/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/platform_specific.h"

#include "base/platform/linux/base_linux_library.h"

#include <QtCore/QObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>
#include <qpa/qplatformwindow_p.h>

#include <algorithm>
#include <cstring>

extern "C" {

typedef int32_t wl_fixed_t;
struct wl_object;
struct wl_array;
struct wl_proxy;
struct wl_display;
struct wl_registry;
struct wl_surface;
struct wl_message;
struct wl_interface;
struct xdg_toplevel;

union wl_argument {
	int32_t i;
	uint32_t u;
	wl_fixed_t f;
	const char *s;
	struct wl_object *o;
	uint32_t n;
	struct wl_array *a;
	int32_t h;
};

struct wl_message {
	const char *name;
	const char *signature;
	const struct wl_interface **types;
};

struct wl_interface {
	const char *name;
	int version;
	int method_count;
	const struct wl_message *methods;
	int event_count;
	const struct wl_message *events;
};

struct wl_registry_listener {
	void (*global)(
		void *data,
		struct wl_registry *registry,
		uint32_t name,
		const char *interface,
		uint32_t version);
	void (*global_remove)(
		void *data,
		struct wl_registry *registry,
		uint32_t name);
};

}

namespace Platform {
namespace {

#if defined QT_FEATURE_wayland && QT_CONFIG(wayland)

constexpr auto kWaylandMarshalFlagDestroy = uint32_t(1);
const auto kTagObjectName = u"_td_xdg_toplevel_tag"_q;

const auto kTagManagerRequests = std::array{
	wl_message{ "destroy", "", nullptr },
	wl_message{ "set_toplevel_tag", "os", nullptr },
	wl_message{ "set_toplevel_description", "os", nullptr },
};
const auto kTagManagerInterface = wl_interface{
	"xdg_toplevel_tag_manager_v1",
	1,
	int(kTagManagerRequests.size()),
	kTagManagerRequests.data(),
	0,
	nullptr,
};

struct WaylandSymbols {
	struct wl_proxy *(*proxyMarshalFlags)(
		struct wl_proxy *proxy,
		uint32_t opcode,
		const struct wl_interface *interface,
		uint32_t version,
		uint32_t flags,
		...) = nullptr;
	int (*proxyAddListener)(
		struct wl_proxy *proxy,
		void (**implementation)(void),
		void *data) = nullptr;
	void (*proxyDestroy)(struct wl_proxy *proxy) = nullptr;
	uint32_t (*proxyGetVersion)(struct wl_proxy *proxy) = nullptr;
	int (*displayRoundtrip)(struct wl_display *display) = nullptr;
	const struct wl_interface *registryInterface = nullptr;

	[[nodiscard]] explicit operator bool() const {
		return proxyMarshalFlags
			&& proxyAddListener
			&& proxyDestroy
			&& proxyGetVersion
			&& displayRoundtrip
			&& registryInterface;
	}
};

[[nodiscard]] const WaylandSymbols *Wayland() {
	static const auto result = [] {
		auto result = WaylandSymbols();
		if (const auto lib = base::Platform::LoadLibrary(
				"libwayland-client.so.0",
				RTLD_NODELETE)) {
			base::Platform::LoadSymbol(
				lib,
				"wl_proxy_marshal_flags",
				result.proxyMarshalFlags);
			base::Platform::LoadSymbol(
				lib,
				"wl_proxy_add_listener",
				result.proxyAddListener);
			base::Platform::LoadSymbol(
				lib,
				"wl_proxy_destroy",
				result.proxyDestroy);
			base::Platform::LoadSymbol(
				lib,
				"wl_proxy_get_version",
				result.proxyGetVersion);
			base::Platform::LoadSymbol(
				lib,
				"wl_display_roundtrip",
				result.displayRoundtrip);
			base::Platform::LoadSymbol(
				lib,
				"wl_registry_interface",
				result.registryInterface);
		}
		return result;
	}();
	return result ? &result : nullptr;
}

void DestroyWaylandProxy(
		const WaylandSymbols &wayland,
		struct wl_proxy *proxy) {
	if (proxy) {
		wayland.proxyMarshalFlags(
			proxy,
			0,
			nullptr,
			wayland.proxyGetVersion(proxy),
			kWaylandMarshalFlagDestroy);
	}
}

struct WaylandRegistryResult {
	uint32_t name = 0;
	uint32_t version = 0;
};

void WaylandRegistryGlobal(
		void *data,
		struct wl_registry *,
		uint32_t name,
		const char *interface,
		uint32_t version) {
	if (!interface
		|| std::strcmp(interface, kTagManagerInterface.name)) {
		return;
	}
	const auto result = static_cast<WaylandRegistryResult*>(data);
	result->name = name;
	result->version = std::min<uint32_t>(
		version,
		kTagManagerInterface.version);
}

void WaylandRegistryGlobalRemove(void *, struct wl_registry *, uint32_t) {
}

auto kWaylandRegistryListener = wl_registry_listener{
	&WaylandRegistryGlobal,
	&WaylandRegistryGlobalRemove,
};

[[nodiscard]] struct wl_proxy *TagManager(struct wl_display *display) {
	struct State {
		struct wl_display *display = nullptr;
		struct wl_proxy *manager = nullptr;
		bool resolved = false;
	};
	static auto state = State();
	const auto wayland = Wayland();
	if (!wayland || !display) {
		return nullptr;
	}
	if (state.display != display) {
		if (state.manager) {
			DestroyWaylandProxy(*wayland, state.manager);
		}
		state = { .display = display };
	}
	if (state.resolved) {
		return state.manager;
	}
	state.resolved = true;

	const auto displayProxy = reinterpret_cast<wl_proxy*>(display);
	const auto registry = reinterpret_cast<wl_registry*>(
		wayland->proxyMarshalFlags(
			displayProxy,
			1,
			wayland->registryInterface,
			wayland->proxyGetVersion(displayProxy),
			0,
			nullptr));
	if (!registry) {
		return nullptr;
	}

	auto result = WaylandRegistryResult();
	wayland->proxyAddListener(
		reinterpret_cast<wl_proxy*>(registry),
		reinterpret_cast<void(**)(void)>(&kWaylandRegistryListener),
		&result);
	wayland->displayRoundtrip(display);
	if (result.name && result.version) {
		state.manager = wayland->proxyMarshalFlags(
			reinterpret_cast<wl_proxy*>(registry),
			0,
			&kTagManagerInterface,
			result.version,
			0,
			result.name,
			kTagManagerInterface.name,
			result.version,
			nullptr);
	}
	wayland->proxyDestroy(reinterpret_cast<wl_proxy*>(registry));
	return state.manager;
}

class ToplevelTag final : public QObject {
public:
	ToplevelTag(not_null<QWindow*> window, QString tag)
	: QObject(window.get())
	, _window(window)
	, _tag(std::move(tag)) {
		setObjectName(kTagObjectName);
		apply();
	}

	void setTag(QString tag) {
		if (_tag == tag) {
			return;
		}
		_tag = std::move(tag);
		apply();
	}

private:
	void apply() {
		using namespace QNativeInterface;
		using namespace QNativeInterface::Private;
		const auto native = qApp->nativeInterface<QWaylandApplication>();
		const auto nativeWindow = _window->nativeInterface<QWaylandWindow>();
		if (!native || !nativeWindow) {
			return;
		}
		if (!nativeWindow->surface()) {
			QObject::connect(
				nativeWindow,
				&QWaylandWindow::surfaceCreated,
				this,
				[this] { apply(); },
				Qt::SingleShotConnection);
			return;
		}
		const auto toplevel = nativeWindow->surfaceRole<xdg_toplevel>();
		if (!toplevel) {
			QObject::connect(
				nativeWindow,
				&QWaylandWindow::surfaceRoleCreated,
				this,
				[this] { apply(); },
				Qt::SingleShotConnection);
			return;
		}
		const auto wayland = Wayland();
		const auto manager = TagManager(native->display());
		if (!wayland || !manager) {
			return;
		}
		const auto utf8 = _tag.toUtf8();
		wayland->proxyMarshalFlags(
			manager,
			1,
			nullptr,
			wayland->proxyGetVersion(manager),
			0,
			toplevel,
			utf8.constData());
	}

	const not_null<QWindow*> _window;
	QString _tag;

};

#endif // wayland

} // namespace

void SetXdgToplevelTag(not_null<QWidget*> window, const QString &tag) {
#if defined QT_FEATURE_wayland && QT_CONFIG(wayland)
	const auto handle = window->windowHandle();
	if (!handle || tag.isEmpty()) {
		return;
	}
	if (handle->findChild<QObject*>(
			kTagObjectName,
			Qt::FindDirectChildrenOnly)) {
		for (const auto child : handle->children()) {
			if (child->objectName() == kTagObjectName) {
				static_cast<ToplevelTag*>(child)->setTag(tag);
				return;
			}
		}
	}
	new ToplevelTag(handle, tag);
#endif // wayland
}

} // namespace Platform
