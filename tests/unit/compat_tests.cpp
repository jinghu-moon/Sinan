#include "compat/symbols.hpp"
#include "hook/feature_admission.hpp"
#include <iostream>
#include <cstring>
#include <functional>

using namespace tabtail;
using namespace tabtail::compat;
namespace {
int checks=0;
void expect(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void rejects(const std::function<void()>& f,const char* message){bool rejected=false;try{f();}catch(...){rejected=true;}expect(rejected,message);}
template<class T>void put(std::string& s,size_t off,const T& value){std::memcpy(s.data()+off,&value,sizeof(value));}
struct Fixture {
    std::string data=std::string(0x2600,'\0');
    Rvas rvas{{"frame_vtable",0x3100},{"host_vtable",0x3200},{"host_interface",0x3300},{"host_window_interface",0x3500},
        {"frame_ctor",0x1100},{"host_ctor",0x1200},{"tab_lookup",0x1300},{"host_window",0x1400},{"location",0x1500},{"title",0x1600}};
    size_t raw(uint32_t rva)const {return rva>=0x5000?rva-0x5000+0x2400:rva>=0x3000?rva-0x3000+0x1400:rva-0x1000+0x400;}
    void code(uint32_t rva,std::initializer_list<unsigned char> bytes){std::copy(bytes.begin(),bytes.end(),data.begin()+raw(rva));}
    void placement(uint32_t at,uint32_t target,unsigned char offset){
        code(at,{0x48,0x8d,0x05,0,0,0,0});put<int32_t>(data,raw(at)+3,static_cast<int32_t>(target-at-7));
        if(!offset)code(at+7,{0x48,0x89,0x01});else code(at+7,{0x48,0x89,0x41,offset});
    }
    Fixture(){
        IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;put(data,0,dos);put<DWORD>(data,0x80,IMAGE_NT_SIGNATURE);
        IMAGE_FILE_HEADER file{};file.Machine=IMAGE_FILE_MACHINE_AMD64;file.NumberOfSections=3;file.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);put(data,0x84,file);
        IMAGE_OPTIONAL_HEADER64 opt{};opt.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;opt.ImageBase=0x180000000;opt.SizeOfImage=0x6000;opt.NumberOfRvaAndSizes=16;
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]={0x3000,sizeof(IMAGE_DEBUG_DIRECTORY)};opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]={0x5000,sizeof(RUNTIME_FUNCTION)};put(data,0x98,opt);
        size_t cursor=0x98+sizeof(opt);
        for(auto s:{Section{0x1000,0x1000,0x400,0x1000,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE},Section{0x3000,0x1000,0x1400,0x1000,IMAGE_SCN_MEM_READ},Section{0x5000,0x200,0x2400,0x200,IMAGE_SCN_MEM_READ}}){
            IMAGE_SECTION_HEADER section{};section.VirtualAddress=s.rva;section.Misc.VirtualSize=s.size;section.SizeOfRawData=s.raw_size;section.PointerToRawData=s.raw;section.Characteristics=s.flags;put(data,cursor,section);cursor+=sizeof(section);
        }
        IMAGE_DEBUG_DIRECTORY debug{};debug.Type=IMAGE_DEBUG_TYPE_CODEVIEW;debug.SizeOfData=42;debug.AddressOfRawData=0x3040;debug.PointerToRawData=static_cast<DWORD>(raw(0x3040));put(data,raw(0x3000),debug);
        data.replace(raw(0x3040),4,"RSDS");GUID guid{0x12345678,0x1234,0x5678,{1,2,3,4,5,6,7,8}};put(data,raw(0x3044),guid);put<DWORD>(data,raw(0x3054),1);
        const char pdb[]="ExplorerFrame.pdb";std::memcpy(data.data()+raw(0x3058),pdb,sizeof(pdb));
        placement(0x1100,0x3100,0);code(0x110a,{0xc3});
        placement(0x1200,0x3200,0);placement(0x120a,0x3300,0x10);placement(0x1215,0x3500,0x70);code(0x1220,{0xc3});
        code(0x1300,{0x48,0x8b,0x99,0xf0,0x02,0,0,0x4c,0x8b,0x81,0xf8,0x02,0,0,0x48,0x2b,0x43,0x10,0x48,0x2b,0x43,0x18,0x48,0x8b,0x33,0xc3,0xcc,0x48,0x83,0xc3,0x28});
        RUNTIME_FUNCTION function{};function.BeginAddress=0x1300;function.EndAddress=0x131f;put(data,raw(0x5000),function);
        code(0x1400,{0x48,0x8b,0x41,0x18,0x48,0x89,0x02,0x33,0xc0,0xc3,0xcc});
        code(0x1500,{0x48,0x83,0xc1,0x28,0x48,0x8b,0x01,0x48,0x8b,0x40,0x58,0xe9,0,0,0,0});
        code(0x1600,{0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0xc3});
        put<uint64_t>(data,raw(0x3300)+4*8,0x180001500);
    }
};
struct FeaturesFixture:Fixture {
    std::vector<std::pair<uint32_t,uint32_t>> functions{{0x1300,31}};
    void function(uint32_t rva,uint32_t size){functions.push_back({rva,size});}
    void jump(uint32_t at,uint32_t target){code(at,{0xe8,0,0,0,0});put<int32_t>(data,raw(at)+1,static_cast<int32_t>(target-at-5));}
    void lea(uint32_t at,std::initializer_list<unsigned char> op,uint32_t disp){code(at,op);put<uint32_t>(data,raw(at)+3,disp);}
    FeaturesFixture(){
        rvas.insert({{"actions_vtable",0x3700},{"select",0x1700},{"remove",0x1800},{"close",0x1880},{"add",0x1900},{"add_index",0x1950},
            {"get_browser",0x1980},{"browser_ctor",0x1a00},{"browser_vtable",0x3800},{"browser_interface",0x3900},
            {"move_selected",0x1b00},{"reorder_iid",0x3a00},{"reorder_service",0x3a10}});
        // Use different offsets from the current system to prove derivation.
        placement(0x110a,0x3700,0);code(0x1111,{0x48,0x89,0x81,0xf0,0,0,0});code(0x1118,{0xc3});function(0x1100,25);
        code(0x1700,{0x4c,0x8b,0xdc,0x49,0x89,0x5b,0x18,0x49,0x89,0x73,0x20,0x48,0x8b,0xd9});
        code(0x170e,{0x48,0x8b,0xbb,0x08,0x02,0,0,0x48,0x2b,0xbb,0x00,0x02,0,0});
        code(0x171c,{0x4c,0x8d,0xb3,0x10,0xff,0xff,0xff,0x41,0x0f,0x10,0x07,0xf3,0x0f,0x7f,0x84,0x24,0xb0,0,0,0,
            0x48,0x8d,0xb3,0x28,0x02,0,0,0x4c,0x8b,0xc6,0x48,0x8d,0x94,0x24,0xb0,0,0,0,0x49,0x8b,0xce});
        jump(0x1745,0x1300);code(0x174a,{0xc3});function(0x1700,75);
        code(0x1800,{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x48,0x8b,0xf1,
            0x48,0x8b,0x86,0x08,0x02,0,0,0x48,0x2b,0x86,0x00,0x02,0,0,0xc3});function(0x1800,29);
        code(0x1880,{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0xc3});
        code(0x1900,{0x48,0x83,0xec,0x38,0x48,0x8b,0x01,0x4c,0x89,0x4c,0x24,0x20,0x41,0x83,0xc9,0xff,0x48,0x8b,0x40,0x48,0xc3});
        code(0x1950,{0x40,0x53,0x56,0x57,0x41,0x54,0xc3});
        code(0x1980,{0x48,0x83,0xec,0x28,0x49,0x83,0x20,0,0xc3});
        placement(0x1a00,0x3800,0);placement(0x1a0a,0x3900,0x18);code(0x1a15,{0xc3});function(0x1a00,22);
        rvas["navigate"]=0x1a40;code(0x1a40,{0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0xc3});
        for(auto [index,target]:{std::pair{3,0x1900},std::pair{4,0x1800},std::pair{5,0x1700},std::pair{9,0x1950}})put<uint64_t>(data,raw(0x3700)+index*8,0x180000000+target);
        put<uint64_t>(data,raw(0x3300)+5*8,0x180001980);
        code(0x1b00,{0x89,0x54,0x24,0x10,0x53,0x57,0x48,0x83,0xec,0x38});
        lea(0x1b0a,{0x4c,0x8d,0x05,0,0,0,0},0x3a00-(0x1b0a+7));
        lea(0x1b11,{0x48,0x8d,0x15,0,0,0,0},0x3a10-(0x1b11+7));
        code(0x1b18,{0x48,0x8b,0x8b,0xb8,0x02,0,0,0x48,0xff,0x15,0,0,0,0,0x0f,0x10,0x83,0x28,0x03,0,0,0x48,0x8b,0x40,0x28});
        jump(0x1b31,0x1c00);code(0x1b36,{0xc3});function(0x1b00,55);
        auto opt=IMAGE_OPTIONAL_HEADER64{};std::memcpy(&opt,data.data()+0x98,sizeof(opt));opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size=static_cast<DWORD>(functions.size()*sizeof(RUNTIME_FUNCTION));put(data,0x98,opt);
        for(size_t i=0;i<functions.size();++i){RUNTIME_FUNCTION f{};f.BeginAddress=functions[i].first;f.EndAddress=f.BeginAddress+functions[i].second;put(data,raw(0x5000)+i*sizeof(f),f);}
    }
};
struct XamlFixture:Fixture {
    Rvas view{{"view_get",0x1600},{"view_vtable",0x3700}};
    Rvas ui{{"tab_ctor",0x1700},{"tab_vtable",0x3800},{"move_tab",0x1900}};
    uint32_t guid_store{},source_load{};
    XamlFixture(){
        code(0x1600,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xc1,0x48,0x83,0x22,0,0xc3});
        put<uint64_t>(data,raw(0x3700)+14*8,0x180001600);
        // Different getter slot, interface and GUID offsets from the baseline.
        placement(0x1700,0x3800,0x18);code(0x170b,{0x4c,0x8b,0xfa,0x4c,0x8b,0xe1});
        uint32_t cursor=0x1711;source_load=cursor;
        const std::vector<std::vector<unsigned char>> loads{{0x44,0x8a,0x42,8},{0x44,0x8a,0x4a,9},{0x44,0x8a,0x52,10},{0x44,0x8a,0x5a,11},
            {0x8a,0x5a,12},{0x40,0x8a,0x7a,13},{0x40,0x8a,0x72,14},{0x40,0x8a,0x6a,15},{0x0f,0xb7,0x52,6},{0x41,0x0f,0xb7,0x4f,4},{0x41,0x8b,7}};
        const std::vector<std::vector<unsigned char>> stores{{0x41,0x89,0x84,0x24},{0x66,0x41,0x89,0x8c,0x24},{0x66,0x41,0x89,0x94,0x24},
            {0x45,0x88,0x84,0x24},{0x45,0x88,0x8c,0x24},{0x45,0x88,0x94,0x24},{0x45,0x88,0x9c,0x24},{0x41,0x88,0x9c,0x24},{0x41,0x88,0xbc,0x24},{0x41,0x88,0xb4,0x24},{0x41,0x88,0xac,0x24}};
        for(auto& op:loads){std::copy(op.begin(),op.end(),data.begin()+raw(cursor));cursor+=static_cast<uint32_t>(op.size());}
        guid_store=cursor;uint32_t deltas[]{0,4,6,8,9,10,11,12,13,14,15};
        for(size_t i=0;i<stores.size();++i){auto& op=stores[i];std::copy(op.begin(),op.end(),data.begin()+raw(cursor));cursor+=static_cast<uint32_t>(op.size());put<uint32_t>(data,raw(cursor),0xb8+deltas[i]);cursor+=4;}
        code(cursor,{0xc3});RUNTIME_FUNCTION f{};f.BeginAddress=0x1700;f.EndAddress=cursor+1;put(data,raw(0x5000),f);
        code(0x1900,{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x57,0x48,0x83,0xec,0x30,0x41,0x8b,0xf8,0xc3});
    }
};

}
int main(){try{
    Fixture fixture;Image image(fixture.data);
    expect(image.pdb_key=="123456781234567801020304050607081","RSDS GUID/Age key uses Windows GUID byte order");
    expect(image.at(0x1600,1)[0]==0x48,"RVA mapped using section raw offset");
    expect(image.function_size(0x1300)==31,"Function boundary comes from x64 unwind table");
    rejects([&]{image.at(0x2000,1);},"Unbacked RVA rejected");rejects([&]{image.at(UINT32_MAX,16);},"Overflow RVA rejected");
    auto profile=validate_profile(image,fixture.rvas);if(!profile.title)std::cerr<<profile.reason<<'\n';
    expect(profile.title&&profile.inventory&&!profile.baseline,"Different image addresses admitted by contract rather than hash whitelist");
    auto manifest=make_manifest(image,fixture.rvas,"test");expect(load_manifest(image,manifest).title,"Verified manifest roundtrip");
    auto corrupt=manifest;corrupt["rvas"]["title"]=0x1610;rejects([&]{load_manifest(image,corrupt);},"Address tampering rejected by checksum");
    corrupt=manifest;corrupt["identity"]["pdb_age"]=2;rejects([&]{load_manifest(image,corrupt);},"Different PDB Age rejected");
    corrupt=manifest;corrupt["schema"]=99;rejects([&]{load_manifest(image,corrupt);},"Future cache schema rejected");
    auto recheck=[&](Json value){value.erase("checksum");value["checksum"]=sha256(value.dump());return value;};
    corrupt=manifest;corrupt["rvas"]["title"]=uint64_t(0x100001600);rejects([&]{load_manifest(image,recheck(corrupt));},"Oversized RVA cannot truncate to a valid address");
    corrupt=manifest;corrupt["rvas"]["title"]=5632.5;rejects([&]{load_manifest(image,recheck(corrupt));},"Floating point RVA rejected");
    corrupt=manifest;corrupt["rvas"]["title"]=-1;rejects([&]{load_manifest(image,recheck(corrupt));},"Negative RVA rejected");
    corrupt=manifest;corrupt["rvas"]["unknown"]=1;rejects([&]{load_manifest(image,recheck(corrupt));},"Unknown symbol entry rejected");
    auto changed=fixture.data;changed[fixture.raw(0x1300)+3]=-32;Image different(changed);
    rejects([&]{load_manifest(different,manifest);},"Same paths with different DLL bytes invalidate cache");
    expect(!validate_profile(different,fixture.rvas).inventory,"Changed table bounds fail closed");
    auto missing=fixture.rvas;missing.erase("host_ctor");expect(!validate_profile(image,missing).inventory,"Missing layout symbol fails closed");
    missing=fixture.rvas;missing.erase("title");profile=validate_profile(image,missing);expect(profile.inventory&&!profile.title,"Missing title symbol degrades only title capability");
    changed=fixture.data;changed[fixture.raw(0x1600)]=-23;Image hooked(changed);profile=validate_profile(hooked,fixture.rvas);expect(profile.inventory&&!profile.title,"Changed title entry preserves read-only inventory");
    auto wrong=fixture.rvas;wrong["title"]=0x3100;profile=validate_profile(image,wrong);expect(profile.inventory&&!profile.title,"Title code symbol in data section degrades title only");
    changed=fixture.data;changed[fixture.raw(0x1214)]=char(0x20);expect(!validate_profile(Image(changed),fixture.rvas).inventory,"Host interface offset change rejected");
    changed=fixture.data;put<uint64_t>(changed,fixture.raw(0x3300)+5*8,0x180001500);expect(!validate_profile(Image(changed),fixture.rvas).inventory,"Ambiguous location vtable membership rejected");
    changed=fixture.data;changed[0x84]=char(0x64);changed[0x85]=-86;rejects([&]{Image invalid(changed);},"ARM64 PE rejected explicitly");
    for(size_t size:{size_t(0),size_t(63),size_t(200),size_t(0x500)})rejects([&]{Image invalid(fixture.data.substr(0,size));},"Truncated PE rejected");
    changed=fixture.data;put<uint32_t>(changed,0x3c,0x7fffffff);rejects([&]{Image invalid(changed);},"Malicious PE header offset rejected");
    changed=fixture.data;changed[fixture.raw(0x3058)]='?';rejects([&]{Image invalid(changed);},"Unsafe PDB identity rejected");
    FeaturesFixture features;auto advanced=validate_profile(Image(features.data),features.rvas);
    if(!advanced.selection||!advanced.close_guard||!advanced.opening||!advanced.navigation_guard||!advanced.reorder)std::cerr<<advanced.failures.dump(2)<<'\n';
    expect(advanced.selection&&advanced.close_guard&&advanced.opening&&advanced.navigation_guard&&advanced.reorder,"Independent native contracts accept a non-baseline image");
    expect(advanced.field("actions")==0xf0&&advanced.field("active_guid")==0x328&&advanced.field("browser_interface")==0x18&&advanced.field("reorder_provider")==0x2b8,"Runtime offsets derived from cross-references instead of baseline constants");
    auto broken=features.rvas;broken.erase("navigate");auto degraded=validate_profile(Image(features.data),broken);
    expect(degraded.selection&&degraded.close_guard&&degraded.opening&&degraded.reorder&&!degraded.navigation_guard,"Missing navigation symbol disables only directory lock");
    expect(feature_admission_error(degraded,FeatureAction::apply,{{"pinned",true}},false).empty(),"Close protection still admitted without navigation capability");
    expect(!feature_admission_error(degraded,FeatureAction::apply,{{"locked",true}},false).empty(),"Directory lock rejected when navigation capability absent");
    expect(feature_admission_error(degraded,FeatureAction::locate,Json::object(),false).empty(),"Tab location still admitted without navigation capability");
    broken=features.rvas;broken.erase("remove");degraded=validate_profile(Image(features.data),broken);
    expect(!degraded.close_guard&&degraded.navigation_guard&&degraded.selection,"Missing close symbol preserves navigation and selection");
    expect(feature_admission_error(degraded,FeatureAction::apply,{{"locked",true}},false).empty(),"Directory lock admitted without close protection");
    expect(!feature_admission_error(degraded,FeatureAction::apply,{{"pinned",true}},false).empty(),"Pin cannot claim protection without close guard");
    broken=features.rvas;broken.erase("select");degraded=validate_profile(Image(features.data),broken);
    expect(!degraded.selection&&!degraded.reorder&&degraded.close_guard&&degraded.navigation_guard,"Missing selection symbol preserves unrelated protections");
    expect(feature_admission_error(degraded,FeatureAction::inspect,Json::object(),false).empty(),"Capability report admitted when active identity is unknown");
    expect(!feature_admission_error(degraded,FeatureAction::apply,{{"locked",true}},true).empty(),"Foreground operation cannot bypass missing active identity");
    expect(feature_admission_error(degraded,FeatureAction::open,{{"select",false}},false).empty()&&!feature_admission_error(degraded,FeatureAction::open,{{"select",true}},false).empty(),"Opening without selection has an independent requirement");
    changed=features.data;put<uint32_t>(changed,features.raw(0x171c)+3,0xfffffff0);degraded=validate_profile(Image(changed),features.rvas);
    expect(!degraded.selection&&degraded.close_guard,"Incorrect this adjustment prevents selection only");
    changed=features.data;put<uint64_t>(changed,features.raw(0x3700)+4*8,0x180001700);degraded=validate_profile(Image(changed),features.rvas);
    expect(!degraded.close_guard&&degraded.selection,"Action vtable mismatch rejects the affected guard");
    changed=features.data;put<uint32_t>(changed,features.raw(0x1b18)+3,0xfffffffc);degraded=validate_profile(Image(changed),features.rvas);
    expect(!degraded.reorder&&degraded.selection&&degraded.close_guard,"Invalid reorder provider offset does not disable selection or close protection");
    changed=features.data;changed[features.raw(0x1a14)]=0x20;degraded=validate_profile(Image(changed),features.rvas);
    expect(degraded.navigation_guard&&degraded.field("browser_interface")==0x20,"Browser interface displacement follows constructor on another layout");
    expect(load_manifest(Image(features.data),make_manifest(Image(features.data),features.rvas,"offline test")).reorder,"Expanded symbol profile roundtrip retains capability-specific contracts");
    XamlFixture xaml;auto view=validate_xaml_profile(Image(xaml.data),xaml.view,Module::view);
    expect(view.xaml_identity&&view.field("view_get_slot")==14,"ViewModel getter slot is derived on a nonbaseline vtable");
    auto ui=validate_xaml_profile(Image(xaml.data),xaml.ui,Module::ui);
    if(!ui.xaml_identity)std::cerr<<ui.failures.dump(2)<<'\n';
    expect(ui.xaml_identity&&ui.reorder_extension&&ui.field("tab_guid")==0xa0,"XAML GUID field derived relative to a moved interface");
    broken=xaml.ui;broken.erase("move_tab");ui=validate_xaml_profile(Image(xaml.data),broken,Module::ui);
    expect(ui.xaml_identity&&!ui.reorder_extension,"Missing reorder method preserves tab appearance identity");
    broken=xaml.ui;broken.erase("tab_ctor");ui=validate_xaml_profile(Image(xaml.data),broken,Module::ui);
    expect(!ui.xaml_identity&&ui.reorder_extension,"Missing tab identity constructor preserves reorder extension");
    changed=xaml.data;changed[xaml.raw(xaml.source_load)+3]=9;ui=validate_xaml_profile(Image(changed),xaml.ui,Module::ui);
    expect(!ui.xaml_identity&&ui.reorder_extension,"Incorrect GUID source field cannot admit appearance mapping");
    changed=xaml.data;put<uint32_t>(changed,xaml.raw(xaml.guid_store)+4,0xbc);ui=validate_xaml_profile(Image(changed),xaml.ui,Module::ui);
    expect(!ui.xaml_identity&&ui.reorder_extension,"Noncontiguous GUID destination fields rejected independently");
    changed=xaml.data;put<uint64_t>(changed,xaml.raw(0x3700)+15*8,0x180001600);
    expect(!validate_xaml_profile(Image(changed),xaml.view,Module::view).xaml_identity,"Ambiguous getter slot fails closed");
    changed=xaml.data;changed[xaml.raw(0x1600)]=-23;
    expect(!validate_xaml_profile(Image(changed),xaml.view,Module::view).xaml_identity,"Changed ViewModel function entry rejected");
    auto xm=make_manifest(Image(xaml.data),xaml.ui,"test");
    expect(load_manifest(Image(xaml.data),xm,Module::ui).xaml_identity,"Module-specific manifest preserves independent XAML contracts");
    rejects([&]{load_manifest(Image(xaml.data),xm,Module::view);},"Module manifest cannot be reused with another module symbol set");
    auto initial=advanced;initial.navigation_guard=initial.reorder=false;initial.rvas.erase("navigate");initial.rvas.erase("move_selected");
    expect(compatible_extension(initial,advanced),"Prepared cache may add capabilities without relocating accepted symbols");
    auto replacement=advanced;replacement.rvas["select"]+=16;expect(!compatible_extension(initial,replacement),"Additive refresh cannot change an accepted function address");
    replacement=advanced;replacement.fields["actions"]+=8;expect(!compatible_extension(initial,replacement),"Additive refresh cannot change an accepted field offset");
    replacement=advanced;replacement.close_guard=false;expect(!compatible_extension(initial,replacement),"Additive refresh cannot revoke an installed guard capability");
    replacement=advanced;replacement.identity["timestamp"]=42;expect(!compatible_extension(initial,replacement),"Running module identity is immutable during additive refresh");
    auto live=current_profile();if(live->baseline){
        expect(live->rvas.contains("navigate")&&live->rvas.contains("get_browser")&&live->rvas.contains("browser_interface")&&live->fields.contains("browser_interface"),"Built-in directory lock profile contains all runtime dependencies");
    }
    std::cout<<checks<<" compatibility checks passed\n";return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}}
