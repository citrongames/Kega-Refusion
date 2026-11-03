#include "WaylandMouseCapture.h"

#include <QWidget>

#if defined(KEGA_WAYLAND_CAPTURE) && QT_CONFIG(wayland)
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <wayland-client.h>
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include <cstring>

struct WaylandMouseCapture::Impl {
    explicit Impl(WaylandMouseCapture* owner) : owner(owner) {
        if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
            return;
        const auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!native || !(display=native->display())) return;
        registry=wl_display_get_registry(display);
        if (registry) {
            static const wl_registry_listener listener{global,globalRemoved};
            wl_registry_add_listener(registry,&listener,this);
        }
        ensurePointer();
    }
    ~Impl() {
        end();
        if (pointer) {
            if (wl_pointer_get_version(pointer)>=3) wl_pointer_release(pointer);
            else wl_pointer_destroy(pointer);
        }
        if (relativeManager) zwp_relative_pointer_manager_v1_destroy(relativeManager);
        if (constraintsManager) zwp_pointer_constraints_v1_destroy(constraintsManager);
        if (registry) wl_registry_destroy(registry);
    }
    void ensurePointer() {
        if (pointer || !display) return;
        const auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!native || !native->seat() || !native->pointer()) return;
        pointer=wl_seat_get_pointer(native->seat());
        if (pointer) {
            static const wl_pointer_listener listener{
                entered,left,ignoredMotion,ignoredButton,ignoredAxis,
#ifdef WL_POINTER_FRAME_SINCE_VERSION
                ignoredFrame,
#endif
#ifdef WL_POINTER_AXIS_SOURCE_SINCE_VERSION
                ignoredAxisSource,
#endif
#ifdef WL_POINTER_AXIS_STOP_SINCE_VERSION
                ignoredAxisStop,
#endif
#ifdef WL_POINTER_AXIS_DISCRETE_SINCE_VERSION
                ignoredAxisDiscrete,
#endif
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
                ignoredAxisValue120,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
                ignoredAxisRelativeDirection,
#endif
#ifdef WL_POINTER_WARP_SINCE_VERSION
                ignoredWarp,
#endif
            };
            wl_pointer_add_listener(pointer,&listener,this);
        }
    }
    bool supported() {
        ensurePointer();
        return pointer && relativeManager && constraintsManager;
    }
    void end() {
        requested=false;
        locked=false;
        lockedSurface=nullptr;
        target.clear();
        if (targetDestroyed) QObject::disconnect(targetDestroyed);
        targetDestroyed={};
        if (relative) { zwp_relative_pointer_v1_destroy(relative);relative=nullptr; }
        if (lock) { zwp_locked_pointer_v1_destroy(lock);lock=nullptr; }
    }
    static void global(void* data,wl_registry* registry,uint32_t name,const char* interface,uint32_t version) {
        auto* self=static_cast<Impl*>(data);
        if (!std::strcmp(interface,zwp_relative_pointer_manager_v1_interface.name) && !self->relativeManager) {
            self->relativeManager=static_cast<zwp_relative_pointer_manager_v1*>(
                wl_registry_bind(registry,name,&zwp_relative_pointer_manager_v1_interface,
                                 version<1?version:1));
            self->relativeGlobal=name;
        } else if (!std::strcmp(interface,zwp_pointer_constraints_v1_interface.name) && !self->constraintsManager) {
            self->constraintsManager=static_cast<zwp_pointer_constraints_v1*>(
                wl_registry_bind(registry,name,&zwp_pointer_constraints_v1_interface,
                                 version<1?version:1));
            self->constraintsGlobal=name;
        }
    }
    static void globalRemoved(void* data,wl_registry*,uint32_t name) {
        auto* self=static_cast<Impl*>(data);
        if(name!=self->relativeGlobal && name!=self->constraintsGlobal)return;
        const bool wasActive=self->requested;
        self->end();
        if(name==self->relativeGlobal) {
            self->relativeGlobal=0;
            if(self->relativeManager) {
                zwp_relative_pointer_manager_v1_destroy(self->relativeManager);
                self->relativeManager=nullptr;
            }
        }
        if(name==self->constraintsGlobal) {
            self->constraintsGlobal=0;
            if(self->constraintsManager) {
                zwp_pointer_constraints_v1_destroy(self->constraintsManager);
                self->constraintsManager=nullptr;
            }
        }
        if(wasActive) emit self->owner->lost();
    }
    static void entered(void* data,wl_pointer*,uint32_t,wl_surface* surface,wl_fixed_t,wl_fixed_t) {
        static_cast<Impl*>(data)->focusSurface=surface;
    }
    static void left(void* data,wl_pointer*,uint32_t,wl_surface* surface) {
        auto* self=static_cast<Impl*>(data);
        if(self->focusSurface==surface)self->focusSurface=nullptr;
        if(self->requested && self->lockedSurface==surface) {
            self->end();
            emit self->owner->lost();
        }
    }
    static void ignoredMotion(void*,wl_pointer*,uint32_t,wl_fixed_t,wl_fixed_t) {}
    static void ignoredButton(void*,wl_pointer*,uint32_t,uint32_t,uint32_t,uint32_t) {}
    static void ignoredAxis(void*,wl_pointer*,uint32_t,uint32_t,wl_fixed_t) {}
#ifdef WL_POINTER_FRAME_SINCE_VERSION
    static void ignoredFrame(void*,wl_pointer*) {}
#endif
#ifdef WL_POINTER_AXIS_SOURCE_SINCE_VERSION
    static void ignoredAxisSource(void*,wl_pointer*,uint32_t) {}
#endif
#ifdef WL_POINTER_AXIS_STOP_SINCE_VERSION
    static void ignoredAxisStop(void*,wl_pointer*,uint32_t,uint32_t) {}
#endif
#ifdef WL_POINTER_AXIS_DISCRETE_SINCE_VERSION
    static void ignoredAxisDiscrete(void*,wl_pointer*,uint32_t,int32_t) {}
#endif
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    static void ignoredAxisValue120(void*,wl_pointer*,uint32_t,int32_t) {}
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    static void ignoredAxisRelativeDirection(void*,wl_pointer*,uint32_t,uint32_t) {}
#endif
#ifdef WL_POINTER_WARP_SINCE_VERSION
    static void ignoredWarp(void*,wl_pointer*,wl_fixed_t,wl_fixed_t) {}
#endif
    static void moved(void* data,zwp_relative_pointer_v1*,uint32_t,uint32_t,
                      wl_fixed_t dx,wl_fixed_t dy,wl_fixed_t,wl_fixed_t) {
        auto* self=static_cast<Impl*>(data);
        if(self->requested && self->locked && self->target)
            emit self->owner->motion(wl_fixed_to_double(dx),wl_fixed_to_double(dy));
    }
    static void didLock(void* data,zwp_locked_pointer_v1*) {
        static_cast<Impl*>(data)->locked=true;
    }
    static void didUnlock(void* data,zwp_locked_pointer_v1*) {
        auto* self=static_cast<Impl*>(data);
        if(!self->requested)return;
        self->end();
        emit self->owner->lost();
    }
    WaylandMouseCapture* owner;
    wl_display* display=nullptr;
    wl_registry* registry=nullptr;
    wl_pointer* pointer=nullptr;
    wl_surface* focusSurface=nullptr;
    wl_surface* lockedSurface=nullptr;
    zwp_relative_pointer_manager_v1* relativeManager=nullptr;
    zwp_pointer_constraints_v1* constraintsManager=nullptr;
    zwp_relative_pointer_v1* relative=nullptr;
    zwp_locked_pointer_v1* lock=nullptr;
    uint32_t relativeGlobal=0,constraintsGlobal=0;
    quint64 requestSerial=0;
    QPointer<QWidget> target;
    QMetaObject::Connection targetDestroyed;
    bool requested=false,locked=false;
};

WaylandMouseCapture::WaylandMouseCapture(QObject* parent)
    : QObject(parent),d_(std::make_unique<Impl>(this)) {}
WaylandMouseCapture::~WaylandMouseCapture()=default;
bool WaylandMouseCapture::supported() const {return d_->supported();}
bool WaylandMouseCapture::begin(QWidget* target) {
    if(active())return d_->target==target;
    if(!target || !target->isVisible() || !target->window()->isActiveWindow() ||
       !d_->supported() || !d_->focusSurface)return false;
    d_->target=target;
    d_->targetDestroyed=connect(target,&QObject::destroyed,this,[this]{end();});
    static const zwp_relative_pointer_v1_listener relativeListener{Impl::moved};
    static const zwp_locked_pointer_v1_listener lockListener{Impl::didLock,Impl::didUnlock};
    d_->relative=zwp_relative_pointer_manager_v1_get_relative_pointer(d_->relativeManager,d_->pointer);
    if(!d_->relative) {end();return false;}
    zwp_relative_pointer_v1_add_listener(d_->relative,&relativeListener,d_.get());
    d_->lock=zwp_pointer_constraints_v1_lock_pointer(
        d_->constraintsManager,d_->focusSurface,d_->pointer,nullptr,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    if(!d_->lock) {end();return false;}
    zwp_locked_pointer_v1_add_listener(d_->lock,&lockListener,d_.get());
    d_->lockedSurface=d_->focusSurface;
    d_->requested=true;
    const quint64 serial=++d_->requestSerial;
    // The protocol permits a compositor to leave a requested lock pending forever.
    QTimer::singleShot(1500,this,[this,serial] {
        if(d_->requested && !d_->locked && d_->requestSerial==serial) {
            d_->end();
            emit lost();
        }
    });
    wl_display_flush(d_->display);
    return true;
}
void WaylandMouseCapture::end() {d_->end();}
bool WaylandMouseCapture::active() const {return d_->requested;}

#else

struct WaylandMouseCapture::Impl {};
WaylandMouseCapture::WaylandMouseCapture(QObject* parent)
    : QObject(parent),d_(std::make_unique<Impl>()) {}
WaylandMouseCapture::~WaylandMouseCapture()=default;
bool WaylandMouseCapture::supported() const {return false;}
bool WaylandMouseCapture::begin(QWidget*) {return false;}
void WaylandMouseCapture::end() {}
bool WaylandMouseCapture::active() const {return false;}

#endif
