/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_topic_navigation.h"

#include "data/data_channel.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_indexed_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

namespace Window {
namespace {

[[nodiscard]] QString SanitizeTagPart(QString text) {
	for (auto &ch : text) {
		if (ch == QChar('\n') || ch == QChar('\r') || ch == QChar(':')) {
			ch = QChar(' ');
		}
	}
	return text.simplified();
}

[[nodiscard]] QString PeerIdPart(not_null<PeerData*> peer) {
	if (peer->isChannel()) {
		return QString::number(peerToChannel(peer->id).bare);
	} else if (peer->isUser()) {
		return QString::number(peerToUser(peer->id).bare);
	} else if (peer->isChat()) {
		return QString::number(peerToChat(peer->id).bare);
	}
	return QString::number(peer->id.value);
}

[[nodiscard]] std::vector<not_null<Data::ForumTopic*>> TopicSequence(
		not_null<Data::Forum*> forum) {
	auto result = std::vector<not_null<Data::ForumTopic*>>();
	for (const auto &row : forum->topicsList()->indexed()->all()) {
		if (const auto topic = row->topic()) {
			result.push_back(topic);
		}
	}
	return result;
}

[[nodiscard]] Data::Forum *ForumFor(
		not_null<SessionController*> controller) {
	if (const auto shown = controller->shownForum().current()) {
		return shown;
	}
	const auto thread = controller->activeChatCurrent().thread();
	if (!thread) {
		return nullptr;
	} else if (const auto topic = thread->asTopic()) {
		return topic->forum();
	}
	return thread->peer()->forum();
}

[[nodiscard]] Data::ForumTopic *AdjacentTopic(
		not_null<Data::Forum*> forum,
		Data::ForumTopic *current,
		int direction) {
	const auto topics = TopicSequence(forum);
	if (topics.empty()) {
		return nullptr;
	}
	int index = -1;
	for (auto i = 0; i != int(topics.size()); ++i) {
		if (topics[i] == current) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		return (direction > 0) ? topics.front() : topics.back();
	}
	const auto next = index + ((direction > 0) ? 1 : -1);
	if (next < 0) {
		return topics.back();
	} else if (next >= int(topics.size())) {
		return topics.front();
	}
	return topics[next];
}

} // namespace

QString XdgToplevelTagFor(SessionController *session) {
	if (!session) {
		return u"Telegram"_q;
	}
	const auto kind = session->isPrimary() ? u"main"_q : u"chat"_q;
	const auto accId = QString::number(session->session().userId().bare);
	auto result = kind + u":"_q + accId;

	PeerData *peer = nullptr;
	Data::ForumTopic *topic = nullptr;
	if (const auto thread = session->activeChatCurrent().thread()) {
		topic = thread->asTopic();
		peer = thread->peer();
	} else if (const auto forum = session->shownForum().current()) {
		peer = forum->peer();
	}
	if (!peer) {
		return result;
	}
	result += u":"_q
		+ SanitizeTagPart(peer->name())
		+ u":"_q
		+ PeerIdPart(peer);
	if (topic) {
		result += u":topic:"_q
			+ SanitizeTagPart(topic->title())
			+ u":"_q
			+ QString::number(topic->rootId().bare);
	}
	return result;
}

bool JumpToAdjacentTopic(
		not_null<SessionController*> controller,
		int direction) {
	const auto forum = ForumFor(controller);
	if (!forum) {
		return false;
	}
	const auto thread = controller->activeChatCurrent().thread();
	const auto current = thread ? thread->asTopic() : nullptr;
	const auto next = AdjacentTopic(forum, current, direction);
	if (!next || next == current) {
		return false;
	}
	controller->showTopic(
		next,
		ShowAtUnreadMsgId,
		SectionShow::Way::ClearStack);
	return true;
}

} // namespace Window
