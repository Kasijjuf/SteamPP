#include <cassert>
#include <string>
#include <vector>

#include "steam++.h"

// unistd.h is broken in MinGW
#ifdef _WIN32
#include <win32/win32dep.h>
#else
#include <unistd.h>
#endif

#define PURPLE_PLUGINS

#include <glib.h>

#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "request.h"
#include "version.h"

using namespace Steam;

struct SteamPurple {
	SteamClient client;
	
	int fd;
	std::vector<unsigned char> read_buffer;
	std::vector<unsigned char> write_buffer;
	std::vector<unsigned char>::size_type read_offset;
	guint watcher;
	
	guint timer;
	std::function<void()> callback;
};

static gboolean plugin_load(PurplePlugin* plugin) {
	return TRUE;
}

static const char* steam_list_icon(PurpleAccount* account, PurpleBuddy* buddy) {
	return "steam";
}

GList* steam_status_types(PurpleAccount* account) {
	GList* types = NULL;
	PurpleStatusType* status;
	
	purple_debug_info("steam", "status_types\n");
	
	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, NULL, "Online", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, NULL, "Offline", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_UNAVAILABLE, NULL, "Busy", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_AWAY, NULL, "Away", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_EXTENDED_AWAY, NULL, "Snoozing", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	
	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, "trade", "Looking to Trade", TRUE, FALSE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, "play", "Looking to Play", TRUE, FALSE, FALSE);
	types = g_list_append(types, status);
	
	return types;
}

static void steam_connect(PurpleAccount* account, SteamPurple* steam) {
	auto &endpoint = servers[rand() % (sizeof(servers) / sizeof(servers[0]))];
	purple_proxy_connect(NULL, account, endpoint.host, endpoint.port, [](gpointer data, gint source, const gchar* error_message) {
		// TODO: check source and error
		assert(source != -1);
		auto steam = reinterpret_cast<SteamPurple*>(data);
		steam->fd = source;
		auto next_length = steam->client.connected();
		steam->read_buffer.resize(next_length);
		steam->watcher = purple_input_add(source, PURPLE_INPUT_READ, [](gpointer data, gint source, PurpleInputCondition cond) {
			auto steam = reinterpret_cast<SteamPurple*>(data);
			auto len = read(source, &steam->read_buffer[steam->read_offset], steam->read_buffer.size() - steam->read_offset);
			purple_debug_info("steam", "read: %i\n", len);
			// len == 0: preceded by a ClientLoggedOff or ClientLogOnResponse, socket should be already closed by us
			// len == -1: TODO
			assert(len > 0);
			steam->read_offset += len;
			if (steam->read_offset == steam->read_buffer.size()) {
				auto next_len = steam->client.readable(steam->read_buffer.data());
				steam->read_offset = 0;
				steam->read_buffer.resize(next_len);
			}
		}, steam);
	}, steam);
}

static void steam_set_steam_guard_token_cb(gpointer data, const gchar* steam_guard_token) {
	auto pc = reinterpret_cast<PurpleConnection*>(data);
	auto account = purple_connection_get_account(pc);
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	
	purple_debug_info("steam", "Got token: %s\n", steam_guard_token);
	steam_connect(account, steam);
	
	std::string token(steam_guard_token);
	steam->client.onHandshake = [steam, account, token] {
		// we just copied the token twice (compiler optimizations notwithstanding) but this is the prettiest way to access it in the lambda
		// yay for capture by move in C++14
		steam->client.LogOn(purple_account_get_username(account), purple_account_get_password(account), nullptr, token.c_str());
	};
}

static void steam_login(PurpleAccount* account) {
	PurpleConnection* pc = purple_account_get_connection(account);
	auto steam = new SteamPurple {
		// SteamClient constructor
		{
			// why use pc instead of steam directly?
			// we can't take steam by value because the pointer is uninitialized at this point
			// we can't take steam by reference because it'll go out of scope when steam_login returns
			
			// write callback
			// we don't actually need account below. it's a workaround for #54947
			// TODO: remove when Ubuntu ships with a newer GCC
			[account, pc](std::size_t length, std::function<void(unsigned char* buffer)> fill) {
				// TODO: check if previous write has finished
				auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
				steam->write_buffer.resize(length);
				fill(steam->write_buffer.data());
				auto len = write(steam->fd, steam->write_buffer.data(), steam->write_buffer.size());
				assert(len == steam->write_buffer.size());
				// TODO: check len
			},
			
			// set_interval callback
			// same as above
			[account, pc](std::function<void()> callback, int timeout) {
				auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
				steam->callback = std::move(callback);
				steam->timer = purple_timeout_add_seconds(timeout, [](gpointer user_data) -> gboolean {
					auto steam = reinterpret_cast<SteamPurple*>(user_data);
					steam->callback();
					return TRUE;
				}, steam);
			}
		}
		// value-initialize the rest
	};
	
	pc->proto_data = steam;
	
	steam->client.onHandshake = [steam, account] {
		auto base64 = purple_account_get_string(account, "sentry_hash", nullptr);
		if (base64) {
			auto hash = purple_base64_decode(base64, NULL);
			steam->client.LogOn(purple_account_get_username(account), purple_account_get_password(account), hash);
			g_free(hash);
		} else {
			steam->client.LogOn(purple_account_get_username(account), purple_account_get_password(account));
		}
	};
	
	steam->client.onLogOn = [account, pc, steam](EResult result, SteamID steamID) {
		auto steamID_string = g_strdup_printf("%" G_GUINT64_FORMAT, steamID);
		
		switch (result) {
		case EResult::OK:
			steam->client.SetPersonaState(EPersonaState::Online);
			purple_connection_set_state(pc, PURPLE_CONNECTED);
			purple_connection_set_display_name(pc, steamID_string);
			break;
		case EResult::AccountLogonDenied:
			purple_request_input(
				/* handle */        NULL,
				/* title */         NULL,
				/* primary */       "Set your Steam Guard Code",
				/* secondary */     "Copy the Steam Guard Code you will have received in your email",
				/* default_value */ NULL,
				/* multiline */     FALSE,
				/* masked */        FALSE,
				/* hint */          "Steam Guard Code",
				/* ok_text */       "OK",
				/* ok_cb */         G_CALLBACK(steam_set_steam_guard_token_cb),
				/* cancel_text */   "Cancel",
				/* cancel_cb */     NULL,
				/* account */       account,
				/* who */           NULL,
				/* conv */          NULL,
				/* user_data */     pc
			);
			// preemptively close the socket because we don't want Pidgin to display a disconnected message
			close(steam->fd);
			purple_input_remove(steam->watcher);
			break;
		case EResult::InvalidPassword:
			purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, "Invalid password");
			break;
		case EResult::ServiceUnavailable:
			purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Steam is down");
			break;
		case EResult::TryAnotherCM:
			purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "This server is down");
			break;
		default:
			purple_debug_error("steam", "Unknown eresult: %i\n", result);
			purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_OTHER_ERROR, "Unknown error");
		}
		
		g_free(steamID_string);
	};
	
	steam->client.onSentry = [account](const unsigned char hash[20]) {
		auto base64 = purple_base64_encode(hash, 20);
		purple_account_set_string(account, "sentry_hash", base64);
		g_free(base64);
	};
	
	steam->client.onUserInfo = [pc, account](SteamID user, SteamID* source, const char* name, EPersonaState* state) {
		auto user_string = g_strdup_printf("%" G_GUINT64_FORMAT, user);
		
		if (source && static_cast<EAccountType>(source->type) == EAccountType::Chat) {
			// either we're joining a chat or something is happening in a chat
			
			// create a dummy group to store aliases if it doesn't exist yet
			auto source_string = g_strdup_printf("%" G_GUINT64_FORMAT, *source);
			auto group = purple_group_new(source_string);
			g_free(source_string);
			
			if (!purple_find_buddy_in_group(account, user_string, group)) {
				// someone new to this chat
				purple_blist_add_buddy(purple_buddy_new(account, user_string, NULL), NULL, group, NULL);
			}
		}
		
		if (name) {
			serv_got_alias(pc, user_string, name);
			if (user == g_ascii_strtoull(purple_connection_get_display_name(pc), NULL, 10))
				purple_account_set_alias(account, name);
		}
		
		if (state) {
			PurpleStatusPrimitive prim;
			switch (*state) {
			case EPersonaState::Offline:
				prim = PURPLE_STATUS_OFFLINE;
				break;
			// these would look the same in Pidgin anyway
			case EPersonaState::Online:
			case EPersonaState::LookingToTrade:
			case EPersonaState::LookingToPlay:
				prim = PURPLE_STATUS_AVAILABLE;
				break;
			case EPersonaState::Busy:
				prim = PURPLE_STATUS_UNAVAILABLE;
				break;
			case EPersonaState::Away:
				prim = PURPLE_STATUS_AWAY;
				break;
			case EPersonaState::Snooze:
				prim = PURPLE_STATUS_EXTENDED_AWAY;
				break;
			}
			purple_prpl_got_user_status(account, user_string, purple_primitive_get_id_from_type(prim), NULL);
		}
		
		g_free(user_string);
	};
	
	steam->client.onChatEnter = [pc](
		SteamID room,
		EChatRoomEnterResponse response,
		const char* name,
		std::size_t member_count,
		const ChatMember members[]
	) {
		if (response == EChatRoomEnterResponse::Success) {
			auto room_string = g_strdup_printf("%" G_GUINT64_FORMAT, room);
			auto convo = serv_got_joined_chat(pc, room.ID, room_string);
			g_free(room_string);
			
			purple_conversation_set_title(convo, name);
			auto chat = purple_conversation_get_chat_data(convo);
			
			while (member_count--) {
				auto member_string = g_strdup_printf("%" G_GUINT64_FORMAT, members[member_count].steamID);
				purple_conv_chat_add_user(
					chat,
					member_string,
					NULL,
					PURPLE_CBFLAGS_NONE, // TODO
					FALSE
				);
				g_free(member_string);
			}
		} else {
			// TODO
		}
	};
	
	steam->client.onChatStateChange = [account, pc](
		SteamID room,
		SteamID acted_by,
		SteamID acted_on,
		EChatMemberStateChange state_change,
		const ChatMember* member
	) {
		auto convo = purple_find_chat(pc, room.ID);
		auto chat = purple_conversation_get_chat_data(convo);
		auto acted_on_string = g_strdup_printf("%" G_GUINT64_FORMAT, acted_on);
		
		if (state_change == EChatMemberStateChange::Entered) {
			purple_conv_chat_add_user(chat, acted_on_string, NULL, PURPLE_CBFLAGS_NONE, TRUE);
		} else {
			// TODO: print reason
			if (acted_on == g_ascii_strtoull(purple_connection_get_display_name(pc), NULL, 10)) {
				// we got kicked or banned
				serv_got_chat_left(pc, room.ID);
			} else {
				purple_conv_chat_remove_user(chat, acted_on_string, NULL);
				
				// remove the respective buddy
				auto group_buddy = purple_find_buddy_in_group(
					account,
					acted_on_string,
					purple_find_group(purple_conversation_get_name(convo))
				);
				purple_blist_remove_buddy(group_buddy);
			}
		}
		
		g_free(acted_on_string);
	};
	
	steam->client.onChatMsg = [pc](SteamID room, SteamID chatter, const char* message) {
		auto chatter_string = g_strdup_printf("%" G_GUINT64_FORMAT, chatter);
		serv_got_chat_in(pc, room.ID, chatter_string, PURPLE_MESSAGE_RECV, message, time(NULL));
		g_free(chatter_string);
	};
	
	steam->client.onPrivateMsg = [pc](SteamID user, const char* message) {
		auto user_string = g_strdup_printf("%" G_GUINT64_FORMAT, user);
		serv_got_im(pc, user_string, message, PURPLE_MESSAGE_RECV, time(NULL));
		g_free(user_string);
	};
	
	steam->client.onTyping = [pc](SteamID user) {
		auto user_string = g_strdup_printf("%" G_GUINT64_FORMAT, user);
		serv_got_typing(pc, user_string, 20, PURPLE_TYPING);
		g_free(user_string);
	};
	
	steam->client.onRelationships = [account, steam](bool incremental, std::size_t count, SteamID users[], EFriendRelationship relationships[]) {
		if (!incremental) {
			// clear list
			auto buddies = purple_blist_get_buddies();
			g_slist_foreach(buddies, [](gpointer data, gpointer user_data) {
				purple_blist_remove_buddy(PURPLE_BUDDY(data));
			}, NULL);
			g_slist_free(buddies);
			
			// request info because we'll only get it for online friends
			steam->client.RequestUserInfo(count, users);
		}
		
		while (count--) {
			auto &user = users[count];
			auto &relationship = relationships[count];
			
			auto user_string = g_strdup_printf("%" G_GUINT64_FORMAT, user);
			
			switch (relationship) {
			case EFriendRelationship::None:
				purple_blist_remove_buddy(purple_find_buddy(account, user_string));
				break;
			case EFriendRelationship::RequestRecipient:
				// TODO
				purple_debug_info("steam", "RequestRecipient not implemented\n");
				break;
			case EFriendRelationship::Friend:
				purple_blist_add_buddy(purple_buddy_new(account, user_string, NULL), NULL, NULL, NULL);
				break;
			case EFriendRelationship::RequestInitiator:
				// TODO
				purple_debug_info("steam", "RequestInitiator not implemented\n");
				break;
			default:
				// TODO
				purple_debug_info("steam", "EFriendRelationship not implemented: %i\n", relationship);
			}
			
			g_free(user_string);
		}
	};
	
	steam_connect(account, steam);
}

static void steam_close(PurpleConnection* pc) {
	// TODO: actually log off maybe
	purple_debug_info("steam", "Closing...\n");
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	close(steam->fd);
	purple_input_remove(steam->watcher);
	purple_timeout_remove(steam->timer);
	delete steam;
}

static GList* steam_chat_info(PurpleConnection* gc) {
	GList* m = NULL;
	struct proto_chat_entry* pce;
	
	pce = g_new0(struct proto_chat_entry, 1);
	pce->label = "SteamID";
	pce->identifier = "steamID";
	pce->required = TRUE;
	m = g_list_append(m, pce);
	
	return m;
}

static int steam_send_im(PurpleConnection* pc, const char* who, const char* message, PurpleMessageFlags flags) {
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	steam->client.SendPrivateMessage(g_ascii_strtoull(who, NULL, 10), message);
	return 1;
}

static unsigned int steam_send_typing(PurpleConnection* pc, const gchar* name, PurpleTypingState state) {
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	if (state == PURPLE_TYPING) {
		steam->client.SendTyping(g_ascii_strtoull(name, NULL, 10));
	}
	return 20;
}

static void steam_set_status(PurpleAccount* account, PurpleStatus* status) {
	PurpleConnection* pc = purple_account_get_connection(account);
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	
	auto prim = purple_status_type_get_primitive(purple_status_get_type(status));
	EPersonaState state;
	
	switch (prim) {
	case PURPLE_STATUS_AVAILABLE:
		state = EPersonaState::Online;
		break;
	case PURPLE_STATUS_UNAVAILABLE:
		state = EPersonaState::Busy;
		break;
	case PURPLE_STATUS_AWAY:
		state = EPersonaState::Away;
		break;
	case PURPLE_STATUS_EXTENDED_AWAY:
		state = EPersonaState::Snooze;
		break;
	case PURPLE_STATUS_INVISIBLE:
		state = EPersonaState::Offline;
		break;
	}
	steam->client.SetPersonaState(state);
}

void steam_join_chat(PurpleConnection* pc, GHashTable* components) {
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	auto steamID_string = reinterpret_cast<const gchar*>(g_hash_table_lookup(components, "steamID"));
	steam->client.JoinChat(g_ascii_strtoull(steamID_string, NULL, 10));
}

void steam_chat_leave(PurpleConnection* pc, int id) {
	auto chat = purple_find_chat(pc, id);
	auto chat_name = purple_conversation_get_name(chat);
	
	auto steam = reinterpret_cast<SteamPurple*>(pc->proto_data);
	steam->client.LeaveChat(g_ascii_strtoull(chat_name, NULL, 10));
	
	// clear the alias storage group
	// despite what the docs imply, you can't remove a non-empty group
	// you can't get the list of buddies in a group either
	
	auto users = purple_conv_chat_get_users(purple_conversation_get_chat_data(chat));
	
	g_list_foreach(users, [](gpointer data, gpointer user_data) {
		auto chat_buddy = reinterpret_cast<PurpleConvChatBuddy*>(data);
		auto chat = reinterpret_cast<PurpleConversation*>(user_data);
		auto group_buddy = purple_find_buddy_in_group(
			purple_conversation_get_account(chat),
			purple_conv_chat_cb_get_name(chat_buddy),
			purple_find_group(purple_conversation_get_name(chat))
		);
		purple_blist_remove_buddy(group_buddy);
	}, chat);
	
	purple_blist_remove_group(purple_find_group(chat_name));
}

int steam_chat_send(PurpleConnection* pc, int id, const char* message, PurpleMessageFlags flags) {
	SteamPurple* steam = (SteamPurple* )pc->proto_data;
	
	// can't reliably reconstruct a full SteamID from an account ID
	steam->client.SendChatMessage(g_ascii_strtoull(purple_conversation_get_name(purple_find_chat(pc, id)), NULL, 10), message);
	
	// the message doesn't get echoed automatically
	serv_got_chat_in(pc, id, purple_connection_get_display_name(pc), PURPLE_MESSAGE_SEND, message, time(NULL));
	
	return 1;
}

char icon_spec_format[] = "png,jpeg";

static PurplePluginProtocolInfo prpl_info = {
	#if PURPLE_VERSION_CHECK(3, 0, 0)
	sizeof(PurplePluginProtocolInfo),       /* struct_size */
	#endif
	
	/* options */
	(PurpleProtocolOptions)NULL,
	
	NULL,                   /* user_splits */
	NULL,                   /* protocol_options */
	/* NO_BUDDY_ICONS */    /* icon_spec */
	{icon_spec_format, 0, 0, 64, 64, 0, PURPLE_ICON_SCALE_DISPLAY}, /* icon_spec */
	steam_list_icon,           /* list_icon */
	NULL, //steam_list_emblem,         /* list_emblems */
	NULL, //steam_status_text,         /* status_text */
	NULL, //steam_tooltip_text,        /* tooltip_text */
	steam_status_types,        /* status_types */
	NULL, //steam_node_menu,           /* blist_node_menu */
	steam_chat_info,           /* chat_info */
	NULL,//steam_chat_info_defaults,  /* chat_info_defaults */
	steam_login,               /* login */
	steam_close,               /* close */
	steam_send_im,             /* send_im */
	NULL,                      /* set_info */
	steam_send_typing,         /* send_typing */
	NULL,//steam_get_info,            /* get_info */
	steam_set_status,          /* set_status */
	NULL, //steam_set_idle,            /* set_idle */
	NULL,                   /* change_passwd */
	NULL, //steam_add_buddy,           /* add_buddy */
	NULL,                   /* add_buddies */
	NULL, //steam_buddy_remove,        /* remove_buddy */
	NULL,                   /* remove_buddies */
	NULL,                   /* add_permit */
	NULL,                   /* add_deny */
	NULL,                   /* rem_permit */
	NULL,                   /* rem_deny */
	NULL,                   /* set_permit_deny */
	steam_join_chat,        /* join_chat */
	NULL,                   /* reject chat invite */
	NULL,//steam_get_chat_name,       /* get_chat_name */
	NULL,                   /* chat_invite */
	steam_chat_leave,       /* chat_leave */
	NULL,                   /* chat_whisper */
	steam_chat_send,        /* chat_send */
	NULL,                   /* keepalive */
	NULL,                   /* register_user */
	NULL,                   /* get_cb_info */
	#if !PURPLE_VERSION_CHECK(3, 0, 0)
	NULL,                   /* get_cb_away */
	#endif
	NULL,                   /* alias_buddy */
	NULL,//steam_fake_group_buddy,    /* group_buddy */
	NULL,//steam_group_rename,        /* rename_group */
	NULL,//steam_buddy_free,          /* buddy_free */
	NULL,//steam_conversation_closed, /* convo_closed */
	NULL,//purple_normalize_nocase,/* normalize */
	NULL,                   /* set_buddy_icon */
	NULL,//steam_group_remove,        /* remove_group */
	NULL,                   /* get_cb_real_name */
	NULL,                   /* set_chat_topic */
	NULL,                   /* find_blist_chat */
	NULL,                   /* roomlist_get_list */
	NULL,                   /* roomlist_cancel */
	NULL,                   /* roomlist_expand_category */
	NULL,                   /* can_receive_file */
	NULL,                   /* send_file */
	NULL,                   /* new_xfer */
	NULL,                   /* offline_message */
	NULL,                   /* whiteboard_prpl_ops */
	NULL,                   /* send_raw */
	NULL,                   /* roomlist_room_serialize */
	NULL,                   /* unregister_user */
	NULL,                   /* send_attention */
	NULL,                   /* attention_types */
	#if (PURPLE_MAJOR_VERSION == 2 && PURPLE_MINOR_VERSION >= 5) || PURPLE_MAJOR_VERSION > 2
	#if PURPLE_MAJOR_VERSION == 2 && PURPLE_MINOR_VERSION >= 5
	sizeof(PurplePluginProtocolInfo), /* struct_size */
	#endif
	NULL, /* steam_get_account_text_table, /* get_account_text_table */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
	#else
	(gpointer) sizeof(PurplePluginProtocolInfo)
	#endif
};

char id[] = "prpl-seishun-steam";
char name[] = "Steam";
char version[] = "1.0";
char summary[] = "";
char description[] = "";
char author[] = "Nicholas <vvnicholas@gmail.com>";
char homepage[] = "https://github.com/seishun/SteamPP";

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_PROTOCOL,
	NULL,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,
	
	id,
	name,
	version,
	
	summary,
	description,
	author,
	homepage,
	
	plugin_load,
	NULL,
	NULL,
	
	NULL,
	&prpl_info,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static void init_plugin(PurplePlugin* plugin) { }

extern "C" {
	PURPLE_INIT_PLUGIN(steam, init_plugin, info)
}
