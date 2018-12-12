/*
bumo is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

bumo is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with bumo.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "election_manager.h"
#include "glue/glue_manager.h"

namespace bumo {
	ElectionManager::ElectionManager() : candidate_mpt_(nullptr), update_validators_(false) {
	}

	ElectionManager::~ElectionManager() {
		if (candidate_mpt_){
			delete candidate_mpt_;
			candidate_mpt_ = nullptr;
		}
	}

	bool ElectionManager::Initialize() {

		candidate_mpt_ = new KVTrie();
		auto batch = std::make_shared<WRITE_BATCH>();
		candidate_mpt_->Init(Storage::Instance().account_db(), batch, General::VALIDATOR_CANDIDATE_PREFIX, 1);

		ValidatorCandidatesLoad();

		// Election configuration
		ElectionConfigure& ecfg = Configure::Instance().election_configure_;

		// Initialize election configuration and write to database when get configuration failed
		if (!ElectionConfigGet(election_config_)) {
			LOG_ERROR("Failed to get election configuration from database!");
			election_config_.set_pledge_amount(ecfg.pledge_amount_);
			election_config_.set_validators_refresh_interval(ecfg.validators_refresh_interval_);
			election_config_.set_coin_to_vote_rate(ecfg.coin_to_vote_rate_);
			election_config_.set_fee_to_vote_rate(ecfg.fee_to_vote_rate_);
			election_config_.set_fee_distribution_rate(ecfg.fee_distribution_rate_);

			ElectionConfigSet(batch, election_config_);
			KeyValueDb *db = Storage::Instance().account_db();
			if (!db->WriteBatch(*batch)) {
				LOG_ERROR("Failed to write election configuration to database(%s)", db->error_desc().c_str());
				return false;
			}
		}

		if (!ReadSharerRate()) {
			LOG_ERROR("Failed to read fees share rate(%s)", election_config_.DebugString().c_str());
			return false;
		}

		// Validator abnormal records
		auto db = Storage::Instance().account_db();
		std::string json_str;
		if (db->Get(General::ABNORMAL_RECORDS, json_str)) {
			abnormal_records_.clear();
			Json::Value abnormal_json;
			if (abnormal_json.fromString(json_str)) {
				LOG_ERROR("Failed to parse json string %s", json_str.c_str());
				UpdateAbnormalRecords(); // reset validator abnormal records
			}
			else {
				for (size_t i = 0; i < abnormal_json.size(); i++) {
					Json::Value& item = abnormal_json[i];
					abnormal_records_.insert(std::make_pair(item["address"].asString(), item["count"].asInt64()));
				}
			}
		}

		TimerNotify::RegisterModule(this);
		StatusModule::RegisterModule(this);
		return true;
	}

	bool ElectionManager::Exit() {
		LOG_INFO("Election manager stoping...");

		if (candidate_mpt_) {
			delete candidate_mpt_;
			candidate_mpt_ = nullptr;
		}
		LOG_INFO("election manager stoped. [OK]");
		return true;
	}

	void ElectionManager::OnTimer(int64_t current_time) {
	}

	void ElectionManager::OnSlowTimer(int64_t current_time) {
	}

	void ElectionManager::GetModuleStatus(Json::Value &data) {
		data["name"] = "election_manager";
		data["configuration"] = Proto2Json(election_config_);
		Json::Value candidates;

		// sort candidate and update validators
		std::multiset<CandidatePtr, PriorityCompare> sorted_candidates;
		std::unordered_map<std::string, CandidatePtr>::iterator it = validator_candidates_.begin();
		for (; it != validator_candidates_.end(); it++) {
			sorted_candidates.insert(it->second);
		}

		// add candidates
		std::multiset<CandidatePtr, PriorityCompare> ::reverse_iterator sit = sorted_candidates.rbegin();
		for (; sit != sorted_candidates.rend(); sit++) {
			CandidatePtr item = *sit;
			Json::Value value = Proto2Json(*item);
			candidates.append(value);
		}
		data["candidates"] = candidates;

		// add abnormal records
		std::unordered_map<std::string, int64_t>::iterator ait = abnormal_records_.begin();
		Json::Value records;
		for (; ait != abnormal_records_.end(); ait++) {
			Json::Value value;
			value["address"] = ait->first;
			value["count"] = ait->second;
			records.append(value);
		}
		data["abnormal_records"] = records;
	}

	void ElectionManager::ElectionConfigSet(std::shared_ptr<WRITE_BATCH> batch, const protocol::ElectionConfig &ecfg) {
		batch->Put(General::ELECTION_CONFIG, ecfg.SerializeAsString());
	}

	const protocol::ElectionConfig& ElectionManager::GetProtoElectionCfg() {
		return election_config_;
	}

	void ElectionManager::SetProtoElectionCfg(const protocol::ElectionConfig& ecfg) {
		election_config_ = ecfg;
	}

	bool ElectionManager::ElectionConfigGet(protocol::ElectionConfig &ecfg) {
		auto db = Storage::Instance().account_db();
		std::string str;

		if (!db->Get(General::ELECTION_CONFIG, str)) {
			return false;
		}
		
		return ecfg.ParseFromString(str);
	}

	int32_t ElectionManager::GetCandidatesNumber() {
		return validator_candidates_.size();
	}

	bool ElectionManager::UpdateElectionConfig(const protocol::ElectionConfig& ecfg) {
		// update coin votes
		if (ecfg.coin_to_vote_rate() != election_config_.coin_to_vote_rate()) {
			std::unordered_map<std::string, CandidatePtr>::iterator it = validator_candidates_.begin();
			for (; it != validator_candidates_.end(); it++) {
				int64_t total_votes_coin = 0;
				if (!utils::SafeIntMul(election_config_.coin_to_vote_rate(), it->second->coin_vote(), total_votes_coin)) {
					LOG_ERROR("Calculation overflowed when coin to vote rate(" FMT_I64 ") * coin vote(" FMT_I64 ").", election_config_.coin_to_vote_rate(), it->second->coin_vote());
					return false;
				}
				it->second->set_coin_vote(total_votes_coin / ecfg.coin_to_vote_rate());
			}
		}

		// Update election configuration storage
		ElectionConfigSet(candidate_mpt_->batch_, ecfg);

		election_config_ = ecfg;

		return ReadSharerRate();
	}

	void ElectionManager::AddAbnormalRecord(const std::string& abnormal_node) {
		std::unordered_map<std::string, int64_t>::iterator it = abnormal_records_.find(abnormal_node);
		if (it != abnormal_records_.end()) {
			it->second++;
		}
		else {
			abnormal_records_.insert(std::make_pair(abnormal_node, 1));
		}
		UpdateAbnormalRecords();
	}

	void ElectionManager::DelAbnormalRecord(const std::string& abnormal_node) {
		auto it = abnormal_records_.find(abnormal_node);
		if (it != abnormal_records_.end()) {
			abnormal_records_.erase(abnormal_node);
			UpdateAbnormalRecords();
		}
	}

	void ElectionManager::GetAbnormalRecords(Json::Value& records) {
		for (std::unordered_map<std::string, int64_t>::iterator it = abnormal_records_.begin();
			it != abnormal_records_.end();
			it++) {
			records[it->first] = it->second;
		}
	}

	void ElectionManager::UpdateAbnormalRecords() {
		Json::Value abnormal_json;
		for (std::unordered_map<std::string, int64_t>::iterator it = abnormal_records_.begin();
			it != abnormal_records_.end();
			it++) {
			Json::Value item;
			item["address"] = it->first;
			item["count"] = it->second;
			abnormal_json.append(item);
		}
		auto batch = candidate_mpt_->batch_;
		if (batch) {
			batch->Put(General::ABNORMAL_RECORDS, abnormal_json.toFastString());
		}
		else {
			LOG_ERROR("Failed to get batch of candidate MPT tree");
		}
		KeyValueDb *db = Storage::Instance().account_db();
		if (!db->WriteBatch(*batch)){
			LOG_ERROR("Failed to write validator abnormal records to database(%s)", db->error_desc().c_str());
		}
	}

	int64_t ElectionManager::CoinToVotes(int64_t coin) {
		return (election_config_.coin_to_vote_rate() < 1) ? 0 : coin / election_config_.coin_to_vote_rate();
	}

	int64_t ElectionManager::FeeToVotes(int64_t fee) {
		return (election_config_.fee_to_vote_rate() < 1) ? 0 : fee / election_config_.fee_to_vote_rate();
	}

	int64_t ElectionManager::GetValidatorsRefreshInterval() {
		return election_config_.validators_refresh_interval();
	}

	bool ElectionManager::ReadSharerRate(){
		std::vector<std::string> vec = utils::String::split(election_config_.fee_distribution_rate(), ":");
		if (vec.size() != SHARER_MAX) {
			return false;
		}

		for (int i = 0; i < SHARER_MAX; i++) {
			uint32_t value = 0;
			if (!utils::String::SafeStoui(vec[i], value)) {
				LOG_ERROR("Failed to convert string(%s) to int", vec[i].c_str());
				return false;
			}
			fee_sharer_rate_.push_back(value);
		}

		return true;
	}

	uint32_t ElectionManager::GetFeesSharerRate(FeeSharerType owner) {
		return fee_sharer_rate_[owner];
	}

	CandidatePtr ElectionManager::GetValidatorCandidate(const std::string& key) {
		CandidatePtr candidate = nullptr;

		auto it = validator_candidates_.find(key);
		if (it != validator_candidates_.end()){
			candidate = it->second;
		}

		return candidate;
	}

	bool  ElectionManager::SetValidatorCandidate(const std::string& key, CandidatePtr value) {
		if (!value) return false;

		try {
			validator_candidates_[key] = value;
		}
		catch (std::exception& e) {
			LOG_ERROR("Caught an exception when set validator candidate, %s", e.what());
			return false;
		}

		return true;
	}

	bool ElectionManager::SetValidatorCandidate(const std::string& key, const protocol::ValidatorCandidate& value) {
		CandidatePtr candidate = std::make_shared<protocol::ValidatorCandidate>(value);
		return SetValidatorCandidate(key, candidate);
	}

	void ElectionManager::DelValidatorCandidate(const std::string& key) {
		validator_candidates_.erase(key);
		to_delete_candidates_.push_back(key);
		DelAbnormalRecord(key);

		protocol::ValidatorSet set = GlueManager::Instance().GetCurrentValidatorSet();
		for (int i = 0; i < set.validators_size(); i++) {
			if (set.validators(i).address() == key) {
				update_validators_ = true;
			}
		}
	}

	bool ElectionManager::ValidatorCandidatesStorage() {
		try {
			for (auto kv : validator_candidates_) {
				candidate_mpt_->Set(kv.first, kv.second->SerializeAsString());
			}

			for (auto node : to_delete_candidates_) {
				candidate_mpt_->Delete(node);
			}

			to_delete_candidates_.clear();
			candidate_mpt_->UpdateHash();
		}
		catch (std::exception& e) {
			LOG_ERROR("Caught an exception when store validator candidate, %s", e.what());
			return false;
		}

		return true;
	}

	bool ElectionManager::ValidatorCandidatesLoad() {

		try {
			std::vector<std::string> entries;
			candidate_mpt_->GetAll("", entries);
			if (!entries.empty()) {
				for (size_t i = 0; i < entries.size(); i++) {
					CandidatePtr candidate = std::make_shared<protocol::ValidatorCandidate>();
					candidate->ParseFromString(entries[i]);
					validator_candidates_[candidate->address()] = candidate;
				}
			}
			else{
				protocol::ValidatorSet set = GlueManager::Instance().GetCurrentValidatorSet();
				for (int i = 0; i < set.validators_size(); i++) {
					CandidatePtr candidate = std::make_shared<protocol::ValidatorCandidate>();
					candidate->set_address(set.validators(i).address());
					candidate->set_pledge(set.validators(i).pledge_coin_amount());
					validator_candidates_[candidate->address()] = candidate;
				}
			}
		}
		catch (std::exception& e) {
			LOG_ERROR("Caught an exception when load validator candidate, %s", e.what());
			return false;
		}

		return true;
	}

	void ElectionManager::UpdateToDB() {
		if (!Storage::Instance().account_db()->WriteBatch(*(candidate_mpt_->batch_))) {
			PROCESS_EXIT("Failed to write accounts to database: %s", Storage::Instance().account_db()->error_desc().c_str());
		}
	}

	bool ElectionManager::DynastyChange(Json::Value& validators_json) {
		if (validator_candidates_.size() == 0) return false;

		// sort candidates and update validators
		std::multiset<CandidatePtr, PriorityCompare> new_validators;
		std::unordered_map<std::string, CandidatePtr>::iterator it = validator_candidates_.begin();
		for (; it != validator_candidates_.end(); it++) {
			int64_t key = it->second->coin_vote() + it->second->fee_vote();
			if (new_validators.size() < General::MAX_VALIDATORS) {
				new_validators.insert(it->second);
			}
			else {
				CandidatePtr min_item = *(new_validators.begin());
				if (min_item->coin_vote() + min_item->fee_vote() <= key) { // compare address if votes are the same
					new_validators.insert(it->second);
					new_validators.erase(new_validators.begin());
				}
			}
		}
		
		// convert new validators to json object
		std::multiset<CandidatePtr, PriorityCompare>::reverse_iterator vit = new_validators.rbegin();
		for (; vit != new_validators.rend(); vit++) 
		{
			Json::Value value;
			value.append((*vit)->address());
			value.append(utils::String::ToString((*vit)->pledge()));
			validators_json.append(value);
		}

		// clear fee_votes
		for (it = validator_candidates_.begin(); it != validator_candidates_.end(); it++) {
			it->second->clear_fee_vote();
		}

		update_validators_ = false;
		return true;
	}
}
