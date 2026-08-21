#include "harmony_permissions_internal.h"

#include <roapi.h>
#include <windows.foundation.h>
#include <windows.devices.geolocation.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <memory>
#include <mutex>

// Where a location actually comes from.
//
// Granting a page the location and then never producing one leaves its promise
// unresolved forever, so this is the other half of that grant: Windows' own
// location service, asked through the Geolocation runtime class.
//
// It runs on a thread of its own, in the multithreaded apartment, because the
// runtime delivers its events on pool threads there and needs no message loop
// to do it. The page host's thread is a single-threaded apartment busy running
// WebKit, and is the wrong place to wait on an access prompt. Fixes cross back
// the way everything else does: published under a lock, then a message to the
// WebKit thread, which hands them to the geolocation manager.

namespace harmony_permissions {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using ABI::Windows::Foundation::DateTime;
using ABI::Windows::Foundation::GetActivationFactory;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Foundation::IAsyncOperationCompletedHandler;
using ABI::Windows::Foundation::IReference;
using ABI::Windows::Foundation::ITypedEventHandler;
using namespace ABI::Windows::Devices::Geolocation;

// 1601 to 1970, in the runtime's own hundred-nanosecond ticks.
constexpr long long kUnixEpochTicks = 116444736000000000LL;
constexpr DWORD kAccessWaitMs = 20000;
constexpr UINT32 kReportIntervalMs = 1000;

std::mutex g_mutex;
HANDLE g_thread { nullptr };
HANDLE g_wake { nullptr };
std::atomic<bool> g_exit { false };
std::atomic<bool> g_wantUpdates { false };
std::atomic<bool> g_wantHighAccuracy { false };

// Owned by the source thread.
ComPtr<IGeolocator> g_locator;
EventRegistrationToken g_positionToken {};
EventRegistrationToken g_statusToken {};
bool g_appliedHighAccuracy { false };

// What the access prompt answered, owned by the handler that writes it.
struct AccessAnswer {
    HANDLE signal { nullptr };
    std::atomic<int> status { 0 };

    ~AccessAnswer()
    {
        if (signal)
            CloseHandle(signal);
    }
};

bool readOptionalDouble(IReference<double>* reference, double& value)
{
    if (!reference)
        return false;
    double read = 0;
    if (FAILED(reference->get_Value(&read)))
        return false;
    value = read;
    return true;
}

void publishFromGeoposition(IGeoposition* position)
{
    if (!position)
        return;

    ComPtr<IGeocoordinate> coordinate;
    if (FAILED(position->get_Coordinate(&coordinate)) || !coordinate)
        return;

    ComPtr<IGeocoordinateWithPoint> withPoint;
    if (FAILED(coordinate.As(&withPoint)) || !withPoint) {
        publishGeolocationFailure("Windows reported a position without coordinates");
        return;
    }

    ComPtr<IGeopoint> point;
    if (FAILED(withPoint->get_Point(&point)) || !point) {
        publishGeolocationFailure("Windows reported a position without coordinates");
        return;
    }

    BasicGeoposition basic {};
    if (FAILED(point->get_Position(&basic))) {
        publishGeolocationFailure("Windows reported a position without coordinates");
        return;
    }

    GeolocationFix fix;
    fix.latitude = basic.Latitude;
    fix.longitude = basic.Longitude;

    double accuracy = 0;
    if (SUCCEEDED(coordinate->get_Accuracy(&accuracy)))
        fix.accuracy = accuracy;

    DateTime timestamp {};
    if (SUCCEEDED(coordinate->get_Timestamp(&timestamp)))
        fix.timestamp = static_cast<double>(timestamp.UniversalTime - kUnixEpochTicks) / 10000000.0;

    ComPtr<IReference<double>> altitudeAccuracy;
    if (SUCCEEDED(coordinate->get_AltitudeAccuracy(&altitudeAccuracy))) {
        double value = 0;
        if (readOptionalDouble(altitudeAccuracy.Get(), value)) {
            fix.hasAltitudeAccuracy = true;
            fix.altitudeAccuracy = value;
            // An altitude is only meaningful with an accuracy for it, which is
            // also how a two-dimensional fix tells itself apart from a three-
            // dimensional one.
            fix.hasAltitude = true;
            fix.altitude = basic.Altitude;
        }
    }

    ComPtr<IReference<double>> heading;
    if (SUCCEEDED(coordinate->get_Heading(&heading))) {
        double value = 0;
        if (readOptionalDouble(heading.Get(), value)) {
            fix.hasHeading = true;
            fix.heading = value;
        }
    }

    ComPtr<IReference<double>> speed;
    if (SUCCEEDED(coordinate->get_Speed(&speed))) {
        double value = 0;
        if (readOptionalDouble(speed.Get(), value)) {
            fix.hasSpeed = true;
            fix.speed = value;
        }
    }

    publishGeolocationFix(fix);
}

// Asks Windows for location access and waits for the answer. The prompt is the
// system's own, shown once per application; a refusal is reported to the page
// as a failure to determine a position, which is what a page's error handler is
// written for.
bool requestAccess()
{
    ComPtr<IGeolocatorStatics> statics;
    HRESULT hr = GetActivationFactory(
        HStringReference(RuntimeClass_Windows_Devices_Geolocation_Geolocator).Get(),
        &statics
    );
    if (FAILED(hr) || !statics) {
        publishGeolocationFailure("this system has no location service");
        return false;
    }

    ComPtr<IAsyncOperation<GeolocationAccessStatus>> operation;
    hr = statics->RequestAccessAsync(&operation);
    if (FAILED(hr) || !operation) {
        publishGeolocationFailure("Windows refused to answer for location access");
        return false;
    }

    // The answer outlives the wait: the handler owns what it writes into, so a
    // prompt a person leaves standing past the timeout cannot write into a
    // stack frame that has already gone.
    auto answer = std::make_shared<AccessAnswer>();
    answer->signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!answer->signal)
        return false;

    hr = operation->put_Completed(
        Callback<IAsyncOperationCompletedHandler<GeolocationAccessStatus>>(
            [answer](IAsyncOperation<GeolocationAccessStatus>* completed, AsyncStatus) -> HRESULT {
                GeolocationAccessStatus result = GeolocationAccessStatus_Unspecified;
                if (completed)
                    completed->GetResults(&result);
                answer->status.store(static_cast<int>(result));
                SetEvent(answer->signal);
                return S_OK;
            }
        ).Get()
    );
    if (FAILED(hr)) {
        publishGeolocationFailure("Windows refused to answer for location access");
        return false;
    }

    if (WaitForSingleObject(answer->signal, kAccessWaitMs) != WAIT_OBJECT_0) {
        publishGeolocationFailure("Windows did not answer for location access");
        return false;
    }

    if (answer->status.load() != static_cast<int>(GeolocationAccessStatus_Allowed)) {
        publishGeolocationFailure("location access is turned off for this app in Windows settings");
        return false;
    }
    return true;
}

void applyAccuracy(bool highAccuracy)
{
    if (!g_locator)
        return;
    g_locator->put_DesiredAccuracy(highAccuracy ? PositionAccuracy_High : PositionAccuracy_Default);
    g_appliedHighAccuracy = highAccuracy;
}

void startLocator()
{
    if (g_locator)
        return;
    if (!requestAccess())
        return;

    ComPtr<IGeolocator> locator;
    HRESULT hr = ABI::Windows::Foundation::ActivateInstance(
        HStringReference(RuntimeClass_Windows_Devices_Geolocation_Geolocator).Get(),
        &locator
    );
    if (FAILED(hr) || !locator) {
        publishGeolocationFailure("this system has no location service");
        return;
    }

    g_locator = locator;
    applyAccuracy(g_wantHighAccuracy.load());
    g_locator->put_ReportInterval(kReportIntervalMs);

    hr = g_locator->add_PositionChanged(
        Callback<ITypedEventHandler<Geolocator*, PositionChangedEventArgs*>>(
            [](IGeolocator*, IPositionChangedEventArgs* args) -> HRESULT {
                if (!args)
                    return S_OK;
                ComPtr<IGeoposition> position;
                if (SUCCEEDED(args->get_Position(&position)))
                    publishFromGeoposition(position.Get());
                return S_OK;
            }
        ).Get(),
        &g_positionToken
    );
    if (FAILED(hr))
        publishGeolocationFailure("Windows would not report position changes");

    g_locator->add_StatusChanged(
        Callback<ITypedEventHandler<Geolocator*, StatusChangedEventArgs*>>(
            [](IGeolocator*, IStatusChangedEventArgs* args) -> HRESULT {
                if (!args)
                    return S_OK;
                PositionStatus status = PositionStatus_NotInitialized;
                if (FAILED(args->get_Status(&status)))
                    return S_OK;
                if (status == PositionStatus_Disabled)
                    publishGeolocationFailure("location is turned off in Windows settings");
                else if (status == PositionStatus_NoData)
                    publishGeolocationFailure("no location data is available here");
                else if (status == PositionStatus_NotAvailable)
                    publishGeolocationFailure("this system has no location service");
                return S_OK;
            }
        ).Get(),
        &g_statusToken
    );

    // The watch only reports movement, so the position already known is asked
    // for outright: a page that asks once should not wait for a person to walk.
    ComPtr<IAsyncOperation<Geoposition*>> pending;
    if (SUCCEEDED(g_locator->GetGeopositionAsync(&pending)) && pending) {
        pending->put_Completed(
            Callback<IAsyncOperationCompletedHandler<Geoposition*>>(
                [](IAsyncOperation<Geoposition*>* operation, AsyncStatus asyncStatus) -> HRESULT {
                    if (!operation || asyncStatus != AsyncStatus::Completed)
                        return S_OK;
                    ComPtr<IGeoposition> position;
                    if (SUCCEEDED(operation->GetResults(&position)))
                        publishFromGeoposition(position.Get());
                    return S_OK;
                }
            ).Get()
        );
    }
}

void stopLocator()
{
    if (!g_locator)
        return;
    if (g_positionToken.value)
        g_locator->remove_PositionChanged(g_positionToken);
    if (g_statusToken.value)
        g_locator->remove_StatusChanged(g_statusToken);
    g_positionToken = EventRegistrationToken {};
    g_statusToken = EventRegistrationToken {};
    g_locator.Reset();
}

DWORD WINAPI sourceThreadMain(LPVOID)
{
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        publishGeolocationFailure("the Windows Runtime could not be started");
        return 0;
    }

    while (!g_exit.load()) {
        WaitForSingleObject(g_wake, INFINITE);
        if (g_exit.load())
            break;

        const bool wanted = g_wantUpdates.load();
        if (wanted && !g_locator)
            startLocator();
        else if (!wanted && g_locator)
            stopLocator();
        else if (g_locator && g_appliedHighAccuracy != g_wantHighAccuracy.load())
            applyAccuracy(g_wantHighAccuracy.load());
    }

    stopLocator();
    if (SUCCEEDED(initialized))
        RoUninitialize();
    return 0;
}

// Starts the source thread on first use. The thread outlives a stop, because a
// page that asks again is the common case and the apartment costs nothing while
// no locator is registered on it.
bool ensureSourceThread()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_thread)
        return true;
    if (g_exit.load())
        return false;

    if (!g_wake) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_wake) {
            setError("the location source could not be started");
            return false;
        }
    }

    g_thread = CreateThread(nullptr, 0, sourceThreadMain, nullptr, 0, nullptr);
    if (!g_thread) {
        setError("the location source could not be started");
        return false;
    }
    return true;
}

} // namespace

void geolocationSourceStart(bool highAccuracy)
{
    g_wantHighAccuracy.store(highAccuracy);
    g_wantUpdates.store(true);
    if (!ensureSourceThread())
        return;
    SetEvent(g_wake);
}

void geolocationSourceStop()
{
    g_wantUpdates.store(false);
    if (g_wake)
        SetEvent(g_wake);
}

void geolocationSourceSetHighAccuracy(bool highAccuracy)
{
    g_wantHighAccuracy.store(highAccuracy);
    if (g_wake)
        SetEvent(g_wake);
}

void geolocationSourceShutdown()
{
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_exit.store(true);
        g_wantUpdates.store(false);
        thread = g_thread;
        g_thread = nullptr;
    }

    if (g_wake)
        SetEvent(g_wake);

    // The wait is bounded so a location prompt a person left standing cannot
    // hold the process open.
    bool joined = true;
    if (thread) {
        joined = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
        CloseHandle(thread);
    }

    // The event outlives a wait that timed out: a thread still inside the access
    // prompt waits on this handle, and handing it back to the process would let
    // something else be opened onto it.
    if (g_wake && joined) {
        CloseHandle(g_wake);
        g_wake = nullptr;
    }
}

} // namespace harmony_permissions
