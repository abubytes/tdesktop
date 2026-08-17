/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/external_control.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_indexed_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "window/window_controller.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"
#include "window/window_topic_navigation.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "base/random.h"
#include "base/flat_map.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Core {
namespace {

constexpr auto kAutomationKey = std::string_view("automation.enabled");

struct WindowEntry {
	not_null<Window::Controller*> controller;
	QWidget *window = nullptr;
};

[[nodiscard]] bool AutomationEnabled() {
	return Core::App().settings().readPref<bool>(kAutomationKey, false);
}

void FillAutomationConfirmBox(
		not_null<Ui::GenericBox*> box,
		const QString &text,
		Fn<void()> enable) {
	box->setTitle(rpl::single(u"Local automation"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(text),
		st::boxLabel));

	const auto cancel = [=] {
		box->closeBox();
	};
	struct Entry {
		QString text;
		Fn<void()> callback;
	};
	auto entries = std::vector<Entry>{
		{ u"Enable"_q, enable },
		{ u"Cancel"_q, cancel },
		{ u"Not sure"_q, cancel },
	};
	for (auto i = int(entries.size()) - 1; i > 0; --i) {
		std::swap(entries[i], entries[base::RandomIndex(i + 1)]);
	}

	const auto content = box->verticalLayout();
	for (const auto &entry : entries) {
		Ui::AddSkip(content);
		const auto button = content->add(
			object_ptr<Ui::RoundButton>(
				content,
				rpl::single(entry.text),
				st::defaultLightButton),
			st::boxRowPadding,
			style::al_justify);
		button->setFullRadius(true);
		button->setClickedCallback(entry.callback);
	}
	box->setStyle(st::localAutomationBox);
}

void RequestEnableAutomation() {
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		return;
	}
	static QPointer<Ui::GenericBox> current;
	if (current) {
		return;
	}
	const auto show = window->uiShow();

	const auto second = [=](not_null<Ui::GenericBox*> box) {
		current = box.get();
		FillAutomationConfirmBox(
			box,
			u"Just to be sure — confirm once more to enable local "
			u"automation."_q,
			[=] {
				Core::App().settings().writePref<bool>(
					kAutomationKey,
					true);
				box->closeBox();
			});
	};
	const auto first = [=](not_null<Ui::GenericBox*> box) {
		current = box.get();
		FillAutomationConfirmBox(
			box,
			u"An external program is trying to control "
			u"Telegram Desktop over the local socket — read open "
			u"windows and activate them.\n\nEnable local "
			u"automation? While it is on, anything running under your "
			u"user account can control the app."_q,
			[=] {
				box->closeBox();
				show->showBox(Box(second));
			});
	};
	show->show(Box(first));
}

[[nodiscard]] QByteArray Pack(QJsonObject object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray Error(const QString &text) {
	auto object = QJsonObject();
	object.insert(u"ok"_q, false);
	object.insert(u"error"_q, text);
	return Pack(object);
}

[[nodiscard]] std::vector<WindowEntry> CollectWindows() {
	auto result = std::vector<WindowEntry>();
	for (const auto widget : QApplication::topLevelWidgets()) {
		const auto controller = App().findWindow(widget);
		if (!controller) {
			continue;
		}
		auto already = false;
		for (const auto &entry : result) {
			if (entry.controller.get() == controller) {
				already = true;
				break;
			}
		}
		if (!already) {
			result.push_back({ controller, widget->window() });
		}
	}
	ranges::sort(result, [](const WindowEntry &a, const WindowEntry &b) {
		if (a.controller->isPrimary() != b.controller->isPrimary()) {
			return a.controller->isPrimary();
		}
		return a.controller.get() < b.controller.get();
	});
	return result;
}

[[nodiscard]] QString SeparateTypeName(Window::SeparateType type) {
	using Type = Window::SeparateType;
	switch (type) {
	case Type::Primary: return u"primary"_q;
	case Type::Archive: return u"archive"_q;
	case Type::Chat: return u"chat"_q;
	case Type::Forum: return u"forum"_q;
	case Type::Community: return u"community"_q;
	case Type::SavedSublist: return u"saved_sublist"_q;
	case Type::SharedMedia: return u"shared_media"_q;
	}
	return u"unknown"_q;
}

void FillWindowObject(
		QJsonObject &object,
		const WindowEntry &entry,
		int index,
		Window::Controller *active) {
	object.insert(u"id"_q, index);
	object.insert(u"title"_q, entry.window->windowTitle());
	object.insert(u"primary"_q, entry.controller->isPrimary());
	object.insert(u"active"_q, (entry.controller.get() == active));
	const auto session = entry.controller->sessionController();
	if (!session) {
		return;
	}
	object.insert(u"type"_q, SeparateTypeName(session->windowId().type));
	object.insert(u"kind"_q, session->isPrimary() ? u"main"_q : u"chat"_q);
	object.insert(
		u"accountId"_q,
		QString::number(session->session().userId().bare));
	object.insert(u"xdgTag"_q, Window::XdgToplevelTagFor(session));
	PeerData *peer = nullptr;
	Data::ForumTopic *topic = nullptr;
	if (const auto thread = session->activeChatCurrent().thread()) {
		topic = thread->asTopic();
		peer = thread->peer();
	} else if (const auto forum = session->shownForum().current()) {
		peer = forum->peer();
	}
	if (peer) {
		object.insert(
			u"peerId"_q,
			QString::number(peer->isChannel()
				? peerToChannel(peer->id).bare
				: peer->id.value));
		object.insert(u"peerTitle"_q, peer->name());
	}
	if (topic) {
		object.insert(u"topicId"_q, QString::number(topic->rootId().bare));
		object.insert(u"topicTitle"_q, topic->title());
	}
}

[[nodiscard]] QByteArray HandleWindows() {
	const auto active = App().activeWindow();
	auto list = QJsonArray();
	auto index = 0;
	for (const auto &entry : CollectWindows()) {
		auto object = QJsonObject();
		FillWindowObject(object, entry, index++, active);
		list.append(object);
	}
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"windows"_q, list);
	return Pack(object);
}

[[nodiscard]] QByteArray HandleActivate(int index) {
	const auto windows = CollectWindows();
	if (index < 0 || index >= int(windows.size())) {
		return Error(u"no such window"_q);
	}
	windows[index].controller->activate();
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"activated"_q, index);
	return Pack(object);
}

[[nodiscard]] base::flat_map<QString, QString> ParseFields(QStringView rest) {
	auto result = base::flat_map<QString, QString>();
	for (const auto &part : QString(rest).split(',')) {
		const auto eq = part.indexOf('=');
		if (eq > 0) {
			result.emplace(part.mid(0, eq).trimmed(), part.mid(eq + 1).trimmed());
		}
	}
	return result;
}

[[nodiscard]] QString FieldAt(
		const base::flat_map<QString, QString> &fields,
		const QString &key) {
	const auto i = fields.find(key);
	return (i != fields.end()) ? i->second : QString();
}

[[nodiscard]] Data::Forum *ForumFromPeer(
		not_null<Main::Session*> session,
		qint64 rawPeer) {
	if (!rawPeer) {
		return nullptr;
	}
	const auto tryId = [&](PeerId id) -> Data::Forum* {
		if (const auto peer = session->data().peerLoaded(id)) {
			return peer->forum();
		}
		return nullptr;
	};
	if (const auto forum = tryId(PeerId(uint64(rawPeer)))) {
		return forum;
	}
	if (rawPeer > 0) {
		return tryId(peerFromChannel(ChannelId(uint64(rawPeer))));
	}
	return nullptr;
}

[[nodiscard]] QByteArray HandleShowTopic(QStringView rest) {
	const auto fields = ParseFields(rest);
	const auto topicId = MsgId(FieldAt(fields, u"topic"_q).toLongLong());
	if (!topicId) {
		return Error(u"topic id required"_q);
	}
	const auto windows = CollectWindows();
	auto windowIndex = -1;
	if (fields.contains(u"window"_q)) {
		windowIndex = FieldAt(fields, u"window"_q).toInt();
		if (windowIndex < 0 || windowIndex >= int(windows.size())) {
			return Error(u"no such window"_q);
		}
	}
	const auto rawPeer = FieldAt(fields, u"peer"_q).toLongLong();
	const auto tryWindow = [&](const WindowEntry &entry) -> bool {
		const auto session = entry.controller->sessionController();
		if (!session) {
			return false;
		}
		auto forum = ForumFromPeer(&session->session(), rawPeer);
		if (!forum) {
			if (const auto shown = session->shownForum().current()) {
				forum = shown;
			}
		}
		if (!forum) {
			const auto thread = session->activeChatCurrent().thread();
			if (thread) {
				if (const auto topic = thread->asTopic()) {
					forum = topic->forum();
				} else {
					forum = thread->peer()->forum();
				}
			}
		}
		if (!forum) {
			return false;
		}
		if (rawPeer) {
			const auto peer = forum->peer();
			const auto matches = peer->isChannel()
				? (peerToChannel(peer->id).bare == uint64(rawPeer)
					|| peer->id.value == uint64(rawPeer))
				: (peer->id.value == uint64(rawPeer));
			if (!matches) {
				return false;
			}
		}
		const auto show = [=](not_null<Data::ForumTopic*> topic) {
			session->showTopic(
				topic,
				ShowAtUnreadMsgId,
				Window::SectionShow::Way::ClearStack);
			entry.controller->activate();
		};
		if (const auto topic = forum->topicFor(topicId)) {
			show(topic);
			return true;
		}
		const auto weak = base::make_weak(session);
		forum->requestTopic(topicId, [=] {
			if (const auto strong = weak.get()) {
				if (const auto topic = forum->topicFor(topicId)) {
					strong->showTopic(
						topic,
						ShowAtUnreadMsgId,
						Window::SectionShow::Way::ClearStack);
					strong->window().activate();
				}
			}
		});
		return true;
	};

	if (windowIndex >= 0) {
		if (!tryWindow(windows[windowIndex])) {
			return Error(u"window has no matching forum"_q);
		}
	} else {
		auto found = false;
		for (const auto &entry : windows) {
			if (tryWindow(entry)) {
				found = true;
				break;
			}
		}
		if (!found) {
			return Error(u"no matching forum window"_q);
		}
	}
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	return Pack(object);
}

[[nodiscard]] QByteArray HandleTopics(QStringView rest) {
	const auto fields = ParseFields(rest);
	const auto windows = CollectWindows();
	auto windowIndex = 0;
	if (fields.contains(u"window"_q)) {
		windowIndex = FieldAt(fields, u"window"_q).toInt();
	}
	if (windowIndex < 0 || windowIndex >= int(windows.size())) {
		return Error(u"no such window"_q);
	}
	const auto session = windows[windowIndex].controller->sessionController();
	if (!session) {
		return Error(u"no session"_q);
	}
	Data::Forum *forum = nullptr;
	if (const auto shown = session->shownForum().current()) {
		forum = shown;
	} else if (const auto thread = session->activeChatCurrent().thread()) {
		if (const auto topic = thread->asTopic()) {
			forum = topic->forum();
		} else {
			forum = thread->peer()->forum();
		}
	}
	if (!forum) {
		return Error(u"window has no forum"_q);
	}
	auto list = QJsonArray();
	for (const auto &row : forum->topicsList()->indexed()->all()) {
		if (const auto topic = row->topic()) {
			auto object = QJsonObject();
			object.insert(u"id"_q, QString::number(topic->rootId().bare));
			object.insert(u"title"_q, topic->title());
			list.append(object);
		}
	}
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"topics"_q, list);
	return Pack(object);
}

[[nodiscard]] QByteArray HandleCycle() {
	const auto windows = CollectWindows();
	if (windows.empty()) {
		return Error(u"no windows"_q);
	}
	const auto active = App().activeWindow();
	auto current = 0;
	for (auto i = 0, count = int(windows.size()); i != count; ++i) {
		if (windows[i].controller.get() == active) {
			current = i;
			break;
		}
	}
	const auto next = (current + 1) % int(windows.size());
	windows[next].controller->activate();
	auto object = QJsonObject();
	object.insert(u"ok"_q, true);
	object.insert(u"activated"_q, next);
	return Pack(object);
}

} // namespace

QByteArray HandleExternalControl(const QString &command) {
	if (!IsAppLaunched()) {
		return Error(u"application is not launched"_q);
	} else if (!AutomationEnabled()) {
		RequestEnableAutomation();
		return Error(u"local automation is disabled — confirm in the "
			u"Telegram window to enable"_q);
	} else if (command == u"automation-off"_q) { // TEMP test helper.
		Core::App().settings().writePref<bool>(kAutomationKey, false);
		auto object = QJsonObject();
		object.insert(u"ok"_q, true);
		return Pack(object);
	} else if (command == u"ping"_q) {
		auto object = QJsonObject();
		object.insert(u"ok"_q, true);
		object.insert(u"result"_q, u"pong"_q);
		return Pack(object);
	} else if (command == u"windows"_q) {
		return HandleWindows();
	} else if (command.startsWith(u"activate:"_q)) {
		return HandleActivate(command.mid(9).toInt());
	} else if (command == u"cycle"_q) {
		return HandleCycle();
	} else if (command.startsWith(u"show-topic:"_q)) {
		return HandleShowTopic(command.mid(11));
	} else if (command.startsWith(u"topics:"_q)) {
		return HandleTopics(command.mid(7));
	} else if (command == u"topics"_q) {
		return HandleTopics({});
	}
	return Error(u"unknown control command"_q);
}

} // namespace Core
