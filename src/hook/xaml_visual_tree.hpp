#pragma once
#include <inspectable.h>
#include <cstdint>

namespace sinan {
// Public WinUI ABI from Microsoft.UI.Xaml.winmd.
struct XamlPoint {float x,y;};
struct XamlRect {float x,y,width,height;};
inline constexpr GUID xaml_dependency_object{0xe7beaee7,0x160e,0x50f7,{0x87,0x89,0xd6,0x34,0x63,0xf9,0x79,0xfa}};
MIDL_INTERFACE("5AECE43C-7651-5BB5-855C-2198496E455E") ITreeHelper : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE FindPoint(XamlPoint,void*,void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindRect(XamlRect,void*,void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAllPoint(XamlPoint,void*,bool,void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAllRect(XamlRect,void*,bool,void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChild(IInspectable*,int32_t,IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChildrenCount(IInspectable*,int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParent(IInspectable*,IInspectable**) = 0;
};
MIDL_INTERFACE("EF9CF81D-A431-50F4-ABF5-3023FE447704") IGridStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundSizingProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BorderBrushProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BorderThicknessProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CornerRadiusProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PaddingProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RowSpacingProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ColumnSpacingProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RowProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRow(IInspectable*,int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRow(IInspectable*,int32_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ColumnProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetColumn(IInspectable*,int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColumn(IInspectable*,int32_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RowSpanProperty(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRowSpan(IInspectable*,int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRowSpan(IInspectable*,int32_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ColumnSpanProperty(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetColumnSpan(IInspectable*,int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColumnSpan(IInspectable*,int32_t) = 0;
};
}
