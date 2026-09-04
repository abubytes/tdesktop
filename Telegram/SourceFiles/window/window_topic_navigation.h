/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/required.h"

namespace Window {

class SessionController;

[[nodiscard]] QString XdgToplevelTagFor(SessionController *session);

bool JumpToAdjacentTopic(
	not_null<SessionController*> controller,
	int direction);

} // namespace Window
