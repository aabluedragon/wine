/* WinRT Windows.Devices.Input PenDevice Implementation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(input);

struct pen_device_statics
{
    IActivationFactory IActivationFactory_iface;
    IPenDeviceStatics IPenDeviceStatics_iface;
    LONG ref;
};

static inline struct pen_device_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct pen_device_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct pen_device_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IPenDeviceStatics ))
    {
        *out = &impl->IPenDeviceStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct pen_device_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct pen_device_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_Devices_Input_PenDevice,
                                ARRAY_SIZE(RuntimeClass_Windows_Devices_Input_PenDevice) - 1, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* PenDevice has no activatable constructor - it is only ever handed out by
     * GetFromPointerId. */
    TRACE( "iface %p, instance %p.\n", iface, instance );
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static inline struct pen_device_statics *impl_from_IPenDeviceStatics( IPenDeviceStatics *iface )
{
    return CONTAINING_RECORD( iface, struct pen_device_statics, IPenDeviceStatics_iface );
}

static HRESULT WINAPI pen_device_statics_QueryInterface( IPenDeviceStatics *iface, REFIID iid, void **out )
{
    struct pen_device_statics *impl = impl_from_IPenDeviceStatics( iface );
    return IActivationFactory_QueryInterface( &impl->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI pen_device_statics_AddRef( IPenDeviceStatics *iface )
{
    struct pen_device_statics *impl = impl_from_IPenDeviceStatics( iface );
    return IActivationFactory_AddRef( &impl->IActivationFactory_iface );
}

static ULONG WINAPI pen_device_statics_Release( IPenDeviceStatics *iface )
{
    struct pen_device_statics *impl = impl_from_IPenDeviceStatics( iface );
    return IActivationFactory_Release( &impl->IActivationFactory_iface );
}

static HRESULT WINAPI pen_device_statics_GetIids( IPenDeviceStatics *iface, ULONG *iid_count, IID **iids )
{
    struct pen_device_statics *impl = impl_from_IPenDeviceStatics( iface );
    return IActivationFactory_GetIids( &impl->IActivationFactory_iface, iid_count, iids );
}

static HRESULT WINAPI pen_device_statics_GetRuntimeClassName( IPenDeviceStatics *iface, HSTRING *class_name )
{
    struct pen_device_statics *impl = impl_from_IPenDeviceStatics( iface );
    return IActivationFactory_GetRuntimeClassName( &impl->IActivationFactory_iface, class_name );
}

static HRESULT WINAPI pen_device_statics_GetTrustLevel( IPenDeviceStatics *iface, TrustLevel *trust_level )
{
    struct pen_device_statics *impl = impl_from_IPenDeviceStatics( iface );
    return IActivationFactory_GetTrustLevel( &impl->IActivationFactory_iface, trust_level );
}

static HRESULT WINAPI pen_device_statics_GetFromPointerId( IPenDeviceStatics *iface, UINT32 pointer_id,
                                                           IPenDevice **value )
{
    TRACE( "iface %p, pointer_id %u, value %p.\n", iface, pointer_id, value );

    if (!value) return E_INVALIDARG;

    /* No pointer input is delivered as pen input, so no pointer id ever names
     * a pen. Windows returns NULL for that, which callers use to tell pen
     * contacts apart from touch and mouse ones. */
    *value = NULL;
    return S_OK;
}

static const struct IPenDeviceStaticsVtbl pen_device_statics_vtbl =
{
    pen_device_statics_QueryInterface,
    pen_device_statics_AddRef,
    pen_device_statics_Release,
    /* IInspectable methods */
    pen_device_statics_GetIids,
    pen_device_statics_GetRuntimeClassName,
    pen_device_statics_GetTrustLevel,
    /* IPenDeviceStatics methods */
    pen_device_statics_GetFromPointerId,
};

static struct pen_device_statics pen_device_statics =
{
    {&factory_vtbl},
    {&pen_device_statics_vtbl},
    1,
};

IActivationFactory *pen_device_factory = &pen_device_statics.IActivationFactory_iface;
