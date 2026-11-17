#pragma once
#include <inspectable.h>
// Public interfaces from Microsoft.UI.Xaml.winmd. ABI prefixes include every
// method preceding the methods used here. QI failure disables only that style.
namespace tabtail::style {
MIDL_INTERFACE("82A4CD9E-435E-5AEB-8C4F-300CECE45CAE") IXamlReader : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Load(HSTRING,IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadWithInitialTemplateValidation(HSTRING,IInspectable**) = 0;
};
MIDL_INTERFACE("7ACDC6A5-F9AA-5D7E-8645-3C48989433A0") IPathSource : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Data(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Data(IInspectable*) = 0;
};
MIDL_INTERFACE("5C8229DB-51CD-5A3B-88EF-1D9A8AC97683") IRenderedPathIcon : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Data(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Data(IInspectable*) = 0;
};
MIDL_INTERFACE("DC102DCC-3BE2-5414-8599-94B6E76EF39B") IGeometryTransform : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Transform(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Transform(IInspectable*) = 0;
};
MIDL_INTERFACE("5AA38ADC-9874-5E0A-8D8E-1574EFC0B88F") IToolTipStatics : public IInspectable {
virtual HRESULT STDMETHODCALLTYPE get_PlacementProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetPlacement(void*, int32_t*) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetPlacement(void*, int32_t) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_PlacementTargetProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetPlacementTarget(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetPlacementTarget(void*, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_ToolTipProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetToolTip(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetToolTip(void*, void*) = 0;
};
MIDL_INTERFACE("B1E3E0F3-112F-5966-87DC-7862D4AD50E5") IAccessibilityStatics : public IInspectable {
virtual HRESULT STDMETHODCALLTYPE get_AcceleratorKeyProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetAcceleratorKey(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetAcceleratorKey(void*, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_AccessKeyProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetAccessKey(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetAccessKey(void*, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_AutomationIdProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetAutomationId(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetAutomationId(void*, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_HelpTextProperty(void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetHelpText(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetHelpText(void*, void*) = 0;
};
MIDL_INTERFACE("E7BEAEE7-160E-50F7-8789-D63463F979FA") IDependencyValues : public IInspectable {
virtual HRESULT STDMETHODCALLTYPE GetValue(void*, void**) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetValue(void*, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE ClearValue(void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE ReadLocalValue(void*, void**) = 0;
};
MIDL_INTERFACE("61DDC651-0383-5D6F-98CE-5C046AAAAA8F") IUnsetValue : public IInspectable {
virtual HRESULT STDMETHODCALLTYPE get_UnsetValue(void**) = 0;
};
}
