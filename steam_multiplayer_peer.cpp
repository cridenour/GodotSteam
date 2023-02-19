#include "steam_multiplayer_peer.h"

#include "godotsteam.h"

VARIANT_ENUM_CAST(SteamMultiplayerPeer::LOBBY_TYPE);
VARIANT_ENUM_CAST(SteamMultiplayerPeer::CHAT_CHANGE);
VARIANT_ENUM_CAST(SteamMultiplayerPeer::LOBBY_STATE);

SteamMultiplayerPeer::SteamMultiplayerPeer() :
		callbackLobbyMessage(this, &SteamMultiplayerPeer::lobby_message_scb),
		callbackLobbyChatUpdate(this, &SteamMultiplayerPeer::lobby_chat_update_scb),
		callbackNetworkMessagesSessionRequest(this, &SteamMultiplayerPeer::network_messages_session_request_scb),
		callbackLobbyJoined(this, &SteamMultiplayerPeer::lobby_joined_scb) {
	ERR_FAIL_COND_MSG(false, "mother fuck qqqqqqqqqqqqqq?");
}

uint64 SteamMultiplayerPeer::get_lobby_id() {
	return lobby_id.ConvertToUint64();
}

void SteamMultiplayerPeer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_lobby", "lobby_type", "max_players"), &SteamMultiplayerPeer::create_lobby, DEFVAL(32));
	ClassDB::bind_method(D_METHOD("connect_lobby", "lobby_id"), &SteamMultiplayerPeer::join_lobby);

	BIND_ENUM_CONSTANT(PRIVATE);
	BIND_ENUM_CONSTANT(FRIENDS_ONLY);
	BIND_ENUM_CONSTANT(PUBLIC);
	BIND_ENUM_CONSTANT(INVISIBLE);
	BIND_ENUM_CONSTANT(PRIVATE_UNIQUE);

	BIND_ENUM_CONSTANT(ENTERED);
	BIND_ENUM_CONSTANT(LEFT);
	BIND_ENUM_CONSTANT(DISCONNECTED);
	BIND_ENUM_CONSTANT(KICKED);
	BIND_ENUM_CONSTANT(BANNED);

	BIND_ENUM_CONSTANT(DISCONNECTED);
	BIND_ENUM_CONSTANT(HOST_PENDING);
	BIND_ENUM_CONSTANT(HOSTING);
	BIND_ENUM_CONSTANT(CLIENT_PENDING);
	BIND_ENUM_CONSTANT(CLIENT);

	// MATCHMAKING SIGNALS //////////////////////
	ADD_SIGNAL(MethodInfo("favorites_list_accounts_updated", PropertyInfo(Variant::INT, "result")));
	ADD_SIGNAL(MethodInfo("favorites_list_changed", PropertyInfo(Variant::DICTIONARY, "favorite")));
	ADD_SIGNAL(MethodInfo("lobby_message", PropertyInfo(Variant::INT, "lobby_id"), PropertyInfo(Variant::INT, "user"), PropertyInfo(Variant::STRING, "message"), PropertyInfo(Variant::INT, "chat_type")));
	ADD_SIGNAL(MethodInfo("lobby_chat_update", PropertyInfo(Variant::INT, "lobby_id"), PropertyInfo(Variant::INT, "changed_id"), PropertyInfo(Variant::INT, "making_change_id"), PropertyInfo(Variant::INT, "chat_state")));
	ADD_SIGNAL(MethodInfo("lobby_created", PropertyInfo(Variant::INT, "connect"), PropertyInfo(Variant::INT, "lobby_id")));
	ADD_SIGNAL(MethodInfo("lobby_data_update"));
	ADD_SIGNAL(MethodInfo("lobby_joined", PropertyInfo(Variant::INT, "lobby"), PropertyInfo(Variant::INT, "permissions"), PropertyInfo(Variant::BOOL, "locked"), PropertyInfo(Variant::INT, "response")));
	ADD_SIGNAL(MethodInfo("lobby_game_created", PropertyInfo(Variant::INT, "lobby_id"), PropertyInfo(Variant::INT, "server_id"), PropertyInfo(Variant::STRING, "server_ip"), PropertyInfo(Variant::INT, "port")));
	ADD_SIGNAL(MethodInfo("lobby_invite", PropertyInfo(Variant::INT, "inviter"), PropertyInfo(Variant::INT, "lobby"), PropertyInfo(Variant::INT, "game")));
	ADD_SIGNAL(MethodInfo("lobby_match_list", PropertyInfo(Variant::ARRAY, "lobbies")));
	ADD_SIGNAL(MethodInfo("lobby_kicked", PropertyInfo(Variant::INT, "lobby_id"), PropertyInfo(Variant::INT, "admin_id"), PropertyInfo(Variant::INT, "due_to_disconnect")));
}

int SteamMultiplayerPeer::get_available_packet_count() const {
	return incoming_packets.size();
}

Error SteamMultiplayerPeer::get_packet(const uint8_t **r_buffer, int &r_buffer_size) {
	ERR_FAIL_COND_V_MSG(incoming_packets.size() == 0, ERR_UNAVAILABLE, "No incoming packets available.");

	delete current_packet;
	current_packet = incoming_packets.front()->get();
	r_buffer_size = current_packet->size;
	*r_buffer = (const uint8_t *)(&current_packet->data);
	incoming_packets.pop_front();
	return OK;
}

int SteamMultiplayerPeer::_get_steam_transfer_flag() {
	// return k_nSteamNetworkingSend_ReliableNoNagle; //todo this helped fix some bugs with unreliable packets
	auto qwer = get_transfer_mode();

	auto agroFlag = k_nSteamNetworkingSend_UseCurrentThread | k_nSteamNetworkingSend_AutoRestartBrokenSession;

	auto autoFlags = (k_nSteamNetworkingSend_NoNagle * no_nagle) |
			(k_nSteamNetworkingSend_NoDelay * no_delay) | agroFlag;

	switch (get_transfer_mode()) {
		case TransferMode::TRANSFER_MODE_RELIABLE:
			return k_nSteamNetworkingSend_Reliable | autoFlags;
			break;
		case TransferMode::TRANSFER_MODE_UNRELIABLE:
			return k_nSteamNetworkingSend_Unreliable | autoFlags;
			break;
		case TransferMode::TRANSFER_MODE_UNRELIABLE_ORDERED:
			ERR_FAIL_V_MSG(
					k_nSteamNetworkingSend_Reliable | autoFlags,
					"UNRELIABLE UNORDERED NOT SUPPORTED! SENDING AS RELIABLE!");
			break;
	}
	ERR_FAIL_V_MSG(-1, "flags error. not sure what happened!?");
}

Error SteamMultiplayerPeer::put_packet(const uint8_t *p_buffer, int p_buffer_size) {
	int transferMode = _get_steam_transfer_flag();
	auto channel = get_transfer_channel() + CHANNEL_MANAGEMENT::SIZE;

	if (target_peer == 0) { //send to ALL
		EResult returnValue = k_EResultOK;
		for (KeyValue<int, Ref<ConnectionData>> &E : connections_by_peer_id) {
			auto errorCode =
					SteamNetworkingMessages()->SendMessageToUser(
							E.value->networkIdentity,
							p_buffer,
							p_buffer_size,
							transferMode,
							channel);
			if (errorCode != k_EResultOK) {
				returnValue = errorCode;
			}
		}
		if (returnValue == k_EResultOK) {
			return Error::OK;
		} else {
			ERR_FAIL_V_MSG(Error(returnValue), String("SteamMultiplayerPeer::put_packet failed with:") + returnValue);
		}
	} else {
		auto errorCode = -1;
		if (connections_by_peer_id.has(target_peer)) {
			errorCode =
					SteamNetworkingMessages()->SendMessageToUser(
							connections_by_peer_id[target_peer]->networkIdentity,
							p_buffer,
							p_buffer_size,
							transferMode,
							channel);
		}
		if (errorCode == k_EResultOK) {
			return Error::OK;
		}
		ERR_FAIL_V_MSG(Error(errorCode), String("SteamMultiplayerPeer::put_packet failed with:") + errorCode);
		// return Error(errorCode);
	}
}

int SteamMultiplayerPeer::get_max_packet_size() const {
	return MAX_STEAM_PACKET_SIZE; //from ENet
}

bool SteamMultiplayerPeer::is_server_relay_supported() const {
	return true;
}

void SteamMultiplayerPeer::set_target_peer(int p_peer_id) {
	target_peer = p_peer_id;
};

int SteamMultiplayerPeer::get_packet_peer() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), 1, "The multiplayer instance isn't currently active.");
	ERR_FAIL_COND_V(incoming_packets.size() == 0, 1);

	auto a = incoming_packets.front()->get()->sender;
	return a == lobby_owner ? 1 : a.GetAccountID();
}

SteamMultiplayerPeer::TransferMode SteamMultiplayerPeer::get_packet_mode() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), TRANSFER_MODE_RELIABLE, "The multiplayer instance isn't currently active.");
	ERR_FAIL_COND_V(incoming_packets.size() == 0, TRANSFER_MODE_RELIABLE);

	return incoming_packets.front()->get()->transfer_mode;
}

int SteamMultiplayerPeer::get_packet_channel() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), 1, "The multiplayer instance isn't currently active.");
	ERR_FAIL_COND_V(incoming_packets.size() == 0, 1);
	int ch = incoming_packets.front()->get()->channel;
	// if (ch >= SYSCH_MAX) { // First 2 channels are reserved.
	// 	return ch - SYSCH_MAX + 1;
	// }
	return ch;
}

void SteamMultiplayerPeer::disconnect_peer(int p_peer, bool p_force) {
	ERR_PRINT("ERROR:: SteamMultiplayerPeer::disconnect_peer not yet implemented");
}

bool SteamMultiplayerPeer::is_server() const {
	return unique_id == 1;
}

#define MAX_MESSAGE_COUNT 255
void SteamMultiplayerPeer::poll() {
	{
		SteamNetworkingMessage_t *messages[MAX_MESSAGE_COUNT];
		int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_MANAGEMENT::SIZE, messages, MAX_MESSAGE_COUNT);
		for (int i = 0; i < count; i++) {
			auto msg = messages[i];
			process_message(msg);
			msg->Release();
		}
	}
	{
		SteamNetworkingMessage_t *messages[MAX_MESSAGE_COUNT];
		int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(CHANNEL_MANAGEMENT::PING_CHANNEL, messages, MAX_MESSAGE_COUNT);
		for (int i = 0; i < count; i++) {
			auto msg = messages[i];
			process_ping(msg);
			msg->Release();
		}
	}
}

void SteamMultiplayerPeer::process_message(const SteamNetworkingMessage_t *msg) {
	auto packet = new Packet;
	packet->channel = 0;
	packet->sender = msg->m_identityPeer.GetSteamID();
	packet->size = msg->GetSize();
	ERR_FAIL_COND_MSG(packet->size > MAX_STEAM_PACKET_SIZE, "PACKET TOO LARGE!");
	auto rawData = (uint8_t *)msg->GetData();
	memcpy(packet->data, rawData, packet->size);
	incoming_packets.push_back(packet);
}
void SteamMultiplayerPeer::process_ping(const SteamNetworkingMessage_t *msg) {
	ERR_FAIL_COND_EDMSG(sizeof(PingPayload) != msg->GetSize(), "wrong size of payload");

	auto data = (PingPayload *)msg->GetData();
}

void SteamMultiplayerPeer::close() {
	ERR_PRINT("ERROR:: SteamMultiplayerPeer::close not yet implemented");
}

int SteamMultiplayerPeer::get_unique_id() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), 0, "The multiplayer instance isn't currently active.");
	return unique_id;
}

SteamMultiplayerPeer::ConnectionStatus SteamMultiplayerPeer::get_connection_status() const {
	if (lobby_state == LOBBY_STATE::NOT_CONNECTED) {
		return ConnectionStatus::CONNECTION_DISCONNECTED;
	} else if (lobby_state == LOBBY_STATE::CLIENT || lobby_state == LOBBY_STATE::HOSTING) {
		return ConnectionStatus::CONNECTION_CONNECTED;
	} else {
		return ConnectionStatus::CONNECTION_CONNECTING;
	}
}

void SteamMultiplayerPeer::add_connection_peer(const CSteamID &steamId, int peer_id) {
	if (steamId == SteamUser()->GetSteamID()) {
		ERR_FAIL_MSG("YOU CAN ADD A PEER THAT IS YOU!");
	} else {
		int peerID = steamId == lobby_owner ? 1 : steamId.GetAccountID();
		Ref<ConnectionData> ccc = Ref<ConnectionData>(memnew(ConnectionData(steamId, steamId.GetAccountID())));
		connections_by_peer_id[peerID] = ccc;
		emit_signal("peer_connected", peerID);
		bool didWork = SteamNetworkingMessages()->AcceptSessionWithUser(ccc->networkIdentity);
		ERR_FAIL_COND_MSG(didWork, "Message failed to join?");
		return;
	}
}

void SteamMultiplayerPeer::add_pending_peer(const CSteamID &steamId) {
	add_connection_peer(steamId,steamId.GetAccountID());
	// pending_connections.push_back(steamId);
}

void SteamMultiplayerPeer::removed_connection_peer(const CSteamID &steamId) {
	for (KeyValue<int, Ref<ConnectionData>> &E : connections_by_peer_id) {
		if (E.value->steamId == steamId) {
			emit_signal("peer_disconnected", E.key);
			auto a = E.value->networkIdentity; //todo: is there something here I should close?
			connections_by_peer_id.erase(E.value->peer_id);
		}
	}
}

Error SteamMultiplayerPeer::create_lobby(LOBBY_TYPE lobby_type, int max_players) {
	ERR_FAIL_COND_V_MSG(lobby_state != LOBBY_STATE::NOT_CONNECTED, ERR_ALREADY_IN_USE, "CANNOT CREATE A LOBBY WHILE IN A LOBBY!");
	if (SteamMatchmaking() != NULL) {
		SteamAPICall_t api_call = SteamMatchmaking()->CreateLobby((ELobbyType)lobby_type, max_players);
		callResultCreateLobby.Set(api_call, this, &SteamMultiplayerPeer::lobby_created);
		unique_id = 1;
		lobby_state = LOBBY_STATE::HOST_PENDING;
		return OK;
	} else {
		ERR_PRINT("`SteamMatchmaking()` is null.");
		return ERR_DOES_NOT_EXIST;
	}
}

void SteamMultiplayerPeer::lobby_created(LobbyCreated_t *lobby_data, bool io_failure) {
	if (io_failure) {
		lobby_state = LOBBY_STATE::NOT_CONNECTED;
		ERR_FAIL_MSG("lobby_created failed? idk wtf is happening");
		// steamworksError("lobby_created");
	} else {
		lobby_state = LOBBY_STATE::HOSTING;
		int connect = lobby_data->m_eResult;
		lobby_id = lobby_data->m_ulSteamIDLobby;
		uint64 lobby = lobby_id.ConvertToUint64();
		emit_signal(SNAME("lobby_created"), connect, lobby); //why do I do this?
	}
}

Error SteamMultiplayerPeer::join_lobby(uint64 lobbyId) {
	ERR_FAIL_COND_V_MSG(lobby_state != LOBBY_STATE::NOT_CONNECTED, ERR_ALREADY_IN_USE, "CANNOT JOIN A LOBBY WHILE IN A LOBBY!");
	if (SteamMatchmaking() != NULL) {
		lobby_state = LOBBY_STATE::CLIENT_PENDING;
		this->lobby_id = lobbyId;
		unique_id = SteamUser()->GetSteamID().GetAccountID();
		// unique_id = generate_unique_id();
		SteamMatchmaking()->JoinLobby(CSteamID(lobbyId));
	}
	return OK;
}

void SteamID::_bind_methods() {
	ClassDB::bind_method(D_METHOD("to_int"), &SteamID::to_int);
	ClassDB::bind_method(D_METHOD("from_int", "id"), &SteamID::from_int);
}

void SteamMultiplayerPeer::lobby_message_scb(LobbyChatMsg_t *call_data) {
	if (lobby_id != call_data->m_ulSteamIDLobby) {
		return;
	}
	Packet *packet = new Packet;

	packet->sender = call_data->m_ulSteamIDUser;
	if (SteamUser()->GetSteamID() == packet->sender) {
		return;
	}
	uint8 chat_type = call_data->m_eChatEntryType;
	// Convert the chat type over
	EChatEntryType type = (EChatEntryType)chat_type;
	// Get the chat message data
	packet->size = SteamMatchmaking()->GetLobbyChatEntry(lobby_id, call_data->m_iChatID, &(packet->sender), &(packet->data), MAX_STEAM_PACKET_SIZE, &type);
	packet->channel = -1;

	incoming_packets.push_back(packet);
};

void SteamMultiplayerPeer::lobby_chat_update_scb(LobbyChatUpdate_t *call_data) {
	if (lobby_id != call_data->m_ulSteamIDLobby) {
		return;
	}
	CSteamID userChanged = CSteamID(call_data->m_ulSteamIDUserChanged);
	switch (CHAT_CHANGE(call_data->m_rgfChatMemberStateChange)) {
		case CHAT_CHANGE::ENTERED:
			if (userChanged != SteamUser()->GetSteamID()) {
				add_pending_peer(userChanged);
			}
			break;
		case CHAT_CHANGE::LEFT:
		case CHAT_CHANGE::DISCONNECTED:
		case CHAT_CHANGE::KICKED:
		case CHAT_CHANGE::BANNED:
			if (userChanged != SteamUser()->GetSteamID()) {
				removed_connection_peer(userChanged);
			}
			break;
	}
};

void SteamMultiplayerPeer::network_messages_session_request_scb(SteamNetworkingMessagesSessionRequest_t *t) {
	//search for lobby member
	CSteamID requester = t->m_identityRemote.GetSteamID();
	int currentLobbySize = SteamMatchmaking()->GetNumLobbyMembers(lobby_id);
	for (int i = 0; i < currentLobbySize; i++) {
		if (SteamMatchmaking()->GetLobbyMemberByIndex(lobby_id, i) == requester) {
			bool didWork = SteamNetworkingMessages()->AcceptSessionWithUser(t->m_identityRemote);
			ERR_FAIL_COND_MSG(didWork, "Message failed to join?");
			return;
		}
	}
	ERR_PRINT(String("CONNECTION ATTEMPTED BY PLAYER NOT IN LOBBY!:") + String::num_uint64(requester.GetAccountID()));
};

void SteamMultiplayerPeer::lobby_joined_scb(LobbyEnter_t *lobbyData) {
	ERR_FAIL_COND_MSG(lobbyData->m_ulSteamIDLobby != this->lobby_id.ConvertToUint64(), "joined a lobby that isn't THIS lobby? this is probably an error? weird");

	if (lobbyData->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess) {
		auto sm = SteamMatchmaking();
		lobby_owner = sm->GetLobbyOwner(lobby_id);
		if (unique_id == 1) {
			//don't do stuff if you're already the host
		} else {
			lobby_state = LOBBY_STATE::CLIENT;
			add_pending_peer(lobby_owner);
		}
		int count = sm->GetNumLobbyMembers(lobby_id);
		for (int i = 0; i < count; i++) {
			CSteamID member = sm->GetLobbyMemberByIndex(lobby_id, i);
			if (member != lobby_owner) { //lobby owner was added above. should happen FIRST
				add_pending_peer(member);
			}
		}

	} else {
		String output = "";
		switch (lobbyData->m_EChatRoomEnterResponse) {
			// k_EChatRoomEnterResponseSuccess: 			output = "k_EChatRoomEnterResponseSuccess"; break;
			case k_EChatRoomEnterResponseDoesntExist:
				output = "Doesn't Exist";
				break;
			case k_EChatRoomEnterResponseNotAllowed:
				output = "Not Allowed";
				break;
			case k_EChatRoomEnterResponseFull:
				output = "Full";
				break;
			case k_EChatRoomEnterResponseError:
				output = "Error";
				break;
			case k_EChatRoomEnterResponseBanned:
				output = "Banned";
				break;
			case k_EChatRoomEnterResponseLimited:
				output = "Limited";
				break;
			case k_EChatRoomEnterResponseClanDisabled:
				output = "Clan Disabled";
				break;
			case k_EChatRoomEnterResponseCommunityBan:
				output = "Community Ban";
				break;
			case k_EChatRoomEnterResponseMemberBlockedYou:
				output = "Member Blocked You";
				break;
			case k_EChatRoomEnterResponseYouBlockedMember:
				output = "You Blocked Member";
				break;
			case k_EChatRoomEnterResponseRatelimitExceeded:
				output = "Ratelimit Exceeded";
				break;
		};
		if (output.length() != 0) {
			ERR_PRINT("Joined lobby failed!" + output);
			lobby_state = LOBBY_STATE::NOT_CONNECTED;
			return;
		}
	}
}