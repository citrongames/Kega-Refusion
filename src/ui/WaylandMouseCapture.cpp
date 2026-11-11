#include "WaylandMouseCapture.h"

#include <QWidget>

#if defined(KEGA_WAYLAND_CAPTURE) && (!defined(QT_FEATURE_wayland) || QT_FEATURE_wayland == 1)
#include <QGuiApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
#include <qpa/qplatformnativeinterface.h>
#endif
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <wayland-client.h>
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include <cstring>
#include <vector>

namespace {
wl_display* qtWaylandDisplay() {
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    auto* native = QGuiApplication::platformNativeInterface();
    return native ? static_cast<wl_display*>(native->nativeResourceForIntegration("display")) : nullptr;
#else
    const auto* native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    return native ? native->display() : nullptr;
#endif
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
wl_seat* qtWaylandSeatWithPointer() {
    const auto* native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    return native && native->pointer() ? native->seat() : nullptr;
}
#endif
}

struct WaylandMouseCapture::Impl {
    explicit Impl(WaylandMouseCapture* owner) : owner(owner) {
        if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
            return;
        if (!(display=qtWaylandDisplay())) return;
        registry=wl_display_get_registry(display);
        if (registry) {
            static const wl_registry_listener listener{global,globalRemoved};
            wl_registry_add_listener(registry,&listener,this);
        }
        ensurePointer();
    }
    ~Impl() {
        end();
        releasePointer();
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        for (const auto& seat : seats) releaseSeat(seat->proxy);
#endif
        if (relativeManager) zwp_relative_pointer_manager_v1_destroy(relativeManager);
        if (constraintsManager) zwp_pointer_constraints_v1_destroy(constraintsManager);
        if (registry) wl_registry_destroy(registry);
    }
    void releasePointer() {
        if (!pointer) return;
        if (wl_pointer_get_version(pointer)>=3) wl_pointer_release(pointer);
        else wl_pointer_destroy(pointer);
        pointer=nullptr;
        focusSurface=nullptr;
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    struct Seat {
        Impl* owner;
        uint32_t name;
        wl_seat* proxy;
        bool hasPointer=false;
    };
    static void seatCapabilities(void* data,wl_seat*,uint32_t capabilities) {
        auto* seat=static_cast<Seat*>(data);
        auto* self=seat->owner;
        seat->hasPointer=(capabilities & WL_SEAT_CAPABILITY_POINTER)!=0;
        if (self->selectedSeat==seat && !seat->hasPointer) {
            const bool wasActive=self->requested;
            self->end();
            self->releasePointer();
            self->selectedSeat=nullptr;
            if (wasActive) emit self->owner->lost();
        }
        self->ensurePointer();
    }
    static void seatName(void*,wl_seat*,const char*) {}
    static void releaseSeat(wl_seat* seat) {
        if (wl_seat_get_version(seat)>=5) wl_seat_release(seat);
        else wl_seat_destroy(seat);
    }
#endif
    void ensurePointer() {
        if (pointer || !display) return;
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        // Qt 6.4's QPA wl_seat/wl_pointer lookup dereferences a missing default
        // input device. Bind a seat ourselves and wait for its pointer capability.
        wl_seat* seat=nullptr;
        for (const auto& candidate : seats) {
            if (candidate->hasPointer) { seat=candidate->proxy; selectedSeat=candidate.get(); break; }
        }
#else
        wl_seat* seat=qtWaylandSeatWithPointer();
#endif
        if (!seat) return;
        pointer=wl_seat_get_pointer(seat);
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
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        } else if (!std::strcmp(interface,wl_seat_interface.name)) {
            auto seat=std::make_unique<Seat>(Seat{self,name,static_cast<wl_seat*>(
                wl_registry_bind(registry,name,&wl_seat_interface,version<5?version:5))});
            static const wl_seat_listener listener{seatCapabilities,seatName};
            wl_seat_add_listener(seat->proxy,&listener,seat.get());
            self->seats.push_back(std::move(seat));
#endif
        }
    }
    static void globalRemoved(void* data,wl_registry*,uint32_t name) {
        auto* self=static_cast<Impl*>(data);
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        for (auto it=self->seats.begin();it!=self->seats.end();++it) {
            if ((*it)->name!=name) continue;
            const bool selected=self->selectedSeat==it->get();
            const bool wasActive=selected && self->requested;
            if (selected) {
                self->end();
                self->releasePointer();
                self->selectedSeat=nullptr;
            }
            releaseSeat((*it)->proxy);
            self->seats.erase(it);
            if (wasActive) emit self->owner->lost();
            if (selected) self->ensurePointer();
            return;
        }
#endif
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
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    std::vector<std::unique_ptr<Seat>> seats;
    Seat* selectedSeat=nullptr;
#endif
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
