#pragma once
#include <eventtoken.h>
#include <inspectable.h>
#include <cstdint>

namespace sinan {
// Public WinUI ABI from Microsoft.UI.Xaml.winmd.
struct XamlPoint {float x,y;};
struct XamlRect {float x,y,width,height;};
inline constexpr GUID xaml_dependency_object{0xe7beaee7,0x160e,0x50f7,{0x87,0x89,0xd6,0x34,0x63,0xf9,0x79,0xfa}};
MIDL_INTERFACE("B7CA8EE3-A07A-5F69-8AB8-BE4E3E4CF0C8") IXamlCommandBar : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_PrimaryCommands(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SecondaryCommands(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CommandBarOverflowPresenterStyle(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_CommandBarOverflowPresenterStyle(IInspectable*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CommandBarTemplateSettings(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DefaultLabelPosition(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DefaultLabelPosition(int32_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_OverflowButtonVisibility(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_OverflowButtonVisibility(int32_t) = 0;
};
MIDL_INTERFACE("F8EB20B4-373E-5327-9942-66A1EA21F5F9") IXamlCommandBarElement : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCompact(bool*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCompact(bool) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsInOverflow(bool*) = 0;
};
// Windows.Foundation.Collections.IVector<ICommandBarElement>.
MIDL_INTERFACE("45A61863-77E4-5FA9-BC53-30E5FE104FB8") IXamlCommandVector : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(uint32_t, IXamlCommandBarElement**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(uint32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetView(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IXamlCommandBarElement*, uint32_t*, bool*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAt(uint32_t, IXamlCommandBarElement*) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAt(uint32_t, IXamlCommandBarElement*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAt(uint32_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE Append(IXamlCommandBarElement*) = 0;
};
MIDL_INTERFACE("71EA1253-DD9C-5509-ACED-2EB045571D53") IXamlCommandsChanged : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable*) = 0;
};
MIDL_INTERFACE("4FC6CE4E-919E-5B90-A071-507B7CC3273C") IXamlObservableCommands : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_VectorChanged(IXamlCommandsChanged*, EventRegistrationToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_VectorChanged(EventRegistrationToken) = 0;
};
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
