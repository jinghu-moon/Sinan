#include "agent/tab_tracker.hpp"
#include "formatter/formatter.hpp"
#include <atomic>
#include <iostream>

using namespace sinan;
namespace {
int checks = 0;
ProcessLifetime assume_running(DWORD, uint64_t) { return ProcessLifetime::running; }
void expect(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F action, const char* text) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; } catch (const PolicyError&) { rejected = true; }
    expect(rejected, text);
}
class Identity final : public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
};
ShellTabObservation observation(uintptr_t child) {
    ShellTabObservation tab;
    tab.window = reinterpret_cast<HWND>(100);
    tab.browser_window = reinterpret_cast<HWND>(child);
    tab.process_id = 42; tab.process_started = 1000;
    tab.com_identity.Attach(new Identity);
    tab.location = {L"D:\\A\\B\\C", L"C"}; tab.location_resolved = true;
    return tab;
}
Json by_id(const Json& all, const std::string& id) {
    for (const auto& tab : all) if (tab.at("tab_id").get<std::string>() == id) return tab;
    throw std::runtime_error("tab missing");
}
}
int main() {
    try {
        Config config;
        {
            bool running=true;
            ShellTabTracker history("closed-history",config,[&](DWORD,uint64_t){
                return running?ProcessLifetime::running:ProcessLifetime::ended;
            });
            for (int i=0;i<21;++i) {
                auto tab=observation(500+i);
                tab.location.filesystem_path=L"D:\\History\\"+std::to_wstring(i);
                tab.native_only=true;
                history.reconcile({{tab},true,{},true});
                history.reconcile({{},true,{}});
                history.reconcile({{},true,{}});
                expect(history.list().size()==1,"COM-only scans cannot retire a native-only tab");
                history.reconcile({{},true,{},true});
                expect(history.list().size()==1,"one authoritative absence does not retire the tab");
                history.reconcile({{},true,{},true});
            }
            expect(history.closed_tabs().size()==20&&history.closed_tabs().front().path==L"D:\\History\\1"&&
                history.closed_tabs().back().path==L"D:\\History\\20","history is capped and ordered by observed close");
            history.forget_closed_tab(history.closed_tabs().back().sequence);
            expect(history.closed_tabs().back().path==L"D:\\History\\19","confirmed reopen removes only its own entry");
            history.reconcile({{observation(600)},true,{}});
            history.reconcile({{},true,{}});
            running=false;
            history.reconcile({{},true,{}});
            expect(history.list().empty()&&history.closed_tabs().size()==19,"Explorer exit does not record a tab close");
        }
        ShellTabTracker tracker("tracker-test", config, assume_running);
        auto a = observation(101), b = observation(102), c = observation(103);
        ShellTabScan scan{{a, b, c}, true, {}};
        tracker.reconcile(scan);
        auto list = tracker.list();
        expect(list.size() == 3, "three real observations tracked independently");
        auto aid = list[0].at("tab_id").get<std::string>();
        auto bid = list[1].at("tab_id").get<std::string>();
        auto cid = list[2].at("tab_id").get<std::string>();
        auto result = tracker.set_depth(aid, 1);
        expect(result.at("policy_saved").get<bool>() && !result.at("title_applied").get<bool>(), "never claim title update");
        tracker.set_depth(bid, std::nullopt);
        tracker.reconcile(scan);
        expect(by_id(tracker.list(), aid).at("depth").get<int>() == 1, "repeated scan preserves override");
        expect(by_id(tracker.list(), bid).at("depth").get<std::string>() == "all", "all persists across scans");
        config.depth = 3; config.revision = 2; tracker.apply_config(config);
        expect(by_id(tracker.list(), cid).at("depth").get<int>() == 3, "unmodified tab follows config");
        expect(by_id(tracker.list(), aid).at("depth").get<int>() == 1, "explicit tab unaffected by config");
        expect(by_id(tracker.list(), bid).at("preview_title").get<std::string>() == "D:\\A\\B\\C", "full-path preview");
        a.location = {L"D:\\One\\Two", L"Two"};
        tracker.reconcile({{a, b, c}, true, {}});
        expect(by_id(tracker.list(), aid).at("preview_title").get<std::string>() == "Two", "navigation uses correct tab override");
        a.location = {std::nullopt, L"此电脑"};
        tracker.reconcile({{a, b, c}, true, {}});
        expect(by_id(tracker.list(), aid).at("preview_title").get<std::string>() == "此电脑", "virtual path doesn't fabricate filesystem path");
        expect(by_id(tracker.list(), aid).at("path").is_null(), "no virtual filesystem path");
        tracker.reset_depth(aid);
        expect(by_id(tracker.list(), aid).at("source").get<std::string>() == "global", "reset inherits");
        tracker.reconcile({{b, c}, false, {"race"}});
        expect(tracker.list().size() == 3, "incomplete scan doesn't retire missing tabs");
        expect(!by_id(tracker.list(), aid).at("observed").get<bool>(), "incomplete scan marks missing tab unobserved");
        rejects([&] { tracker.set_depth(aid, 2); }, "incomplete observation cannot target missing tab");
        tracker.reconcile({{b, c}, true, {}});
        expect(tracker.list().size() == 3, "one complete absence waits for confirmation");
        rejects([&] { tracker.set_depth(aid, 2); }, "unobserved tab cannot be targeted");
        tracker.reconcile({{b, c}, true, {}});
        expect(tracker.list().size() == 2, "second complete absence retires tab");
        expect(tracker.closed_tabs().empty(), "virtual locations are not saved as filesystem tabs");
        rejects([&] { tracker.reset_depth(aid); }, "retired token rejected");
        auto reused = observation(101); // Same HWND/path, new COM identity.
        tracker.reconcile({{reused, b, c}, true, {}});
        auto after = tracker.list();
        std::string new_id;
        for (const auto& tab : after) if (tab.at("browser_window").get<uintptr_t>() == 101) new_id = tab.at("tab_id").get<std::string>();
        expect(!new_id.empty() && new_id != aid, "reused child handle gets fresh token");
        expect(by_id(after, new_id).at("source").get<std::string>() == "global", "new lifetime does not inherit old override");
        b.location = {std::nullopt, L"位置暂不可读"}; b.location_resolved = false; b.warning = "busy";
        tracker.reconcile({{reused, b, c}, true, {}});
        auto unresolved = by_id(tracker.list(), bid);
        expect(unresolved.at("path").is_null() && !unresolved.at("location_resolved").get<bool>(), "unresolved navigation clears stale path");
        expect(unresolved.at("warning").get<std::string>() == "busy", "location warning exposed");
        expect(unresolved.at("depth").get<std::string>() == "all", "unresolved path does not drop policy");
        {
            ShellTabTracker live("directory-target",config,assume_running);
            auto first=observation(401),second=observation(402),third=observation(403),unmapped=observation(404);
            const auto foreground=reinterpret_cast<HWND>(200);
            for(auto* tab:{&first,&second,&third}) {
                if(tab!=&first)tab->window=foreground;
                tab->native=NativeTab{};
                tab->native->window=tab->window;
                tab->native->browser_window=tab->browser_window;
            }
            live.reconcile({{unmapped,first,second,third},true,{}});
            const auto tabs=live.list();
            const auto second_id=tabs[2].at("tab_id").get<std::string>();
            auto chosen=live.directory_target(L"d:/a/b/c/",foreground);
            expect(chosen.tab_id==second_id&&chosen.foreground_anchor&&chosen.foreground_anchor->browser_window==second.browser_window,"first verified match in the foreground window wins");
            expect(live.directory_target(L"D:\\A\\B\\C",nullptr).tab_id==tabs[1].at("tab_id").get<std::string>(),"verified background match wins over unmapped match");
            live.reconcile({{unmapped},false,{"race"}});
            chosen=live.directory_target(L"D:\\A\\B\\C",foreground);
            expect(chosen.tab_id==tabs[0].at("tab_id").get<std::string>()&&!chosen.foreground_anchor,"unobserved tabs are ignored and an unmapped match still prevents a duplicate open");
            unmapped.location={std::nullopt,L""};unmapped.location_resolved=false;
            live.reconcile({{unmapped},true,{}});
            chosen=live.directory_target(L"D:\\A\\B\\C",foreground);
            expect(chosen.tab_id.empty()&&!chosen.locations_resolved,"unknown tab location prevents treating a directory as absent");
        }
        {
            Config rule_config;
            DirectoryRule source;source.id="source";source.patterns={L"D:\\A\\**\\src"};source.depth=3;
            DirectoryRule cache;cache.id="cache";cache.patterns={L"D:\\A\\**\\cache\\**"};cache.native=true;
            rule_config.rules={source,cache};
            ShellTabTracker live("rules-bridge",rule_config,assume_running);
            auto first=observation(301),second=observation(302);
            first.location={L"D:\\A\\Project\\src",L"src"};second.location={L"D:\\A\\Project\\cache",L"cache"};
            first.native=NativeTab{};first.native->browser_window=first.browser_window;
            second.native=NativeTab{};second.native->browser_window=second.browser_window;
            auto reconcile_rules=[&]{live.reconcile({{first,second},true,{}});};reconcile_rules();
            auto listing=live.list();auto x=listing[0]["tab_id"].get<std::string>(),y=listing[1]["tab_id"].get<std::string>();
            ShellTabTracker::TitleTransport bridge=[&](const NativeTab& tab,BridgeAction,const RuntimePolicy& policy) {
                const auto& location=tab.browser_window==first.browser_window?first.location:second.location;
                auto decoded=runtime_policy_from_json(runtime_policy_json(policy));
                auto effective=decoded.resolve(location.filesystem_path);
                expect(decoded.rules->definitions()==rule_config.rules,"full directory rules cross bridge");
                return BridgeResult{S_OK,effective.native?location.original_title:format_title(*location.filesystem_path,effective.display).text};
            };
            auto verify=[&]{
                expect(live.synchronize_titles(true,bridge).at("complete"),"rule bridge ACK");
                for(const auto& tab:live.list())expect(tab.at("preview_title")==tab.at("applied_title"),"agent/bridge title parity");
            };
            verify();expect(by_id(live.list(),y).at("action")=="native","native rule represented in listing");
            live.set_depth(x,1);first.location=second.location;reconcile_rules();verify();
            expect(by_id(live.list(),x).at("source")=="tab","override wins after navigation into native rule");
            live.reset_depth(x);verify();
            expect(by_id(live.list(),x).at("action")=="native","reset restores native action");
            first.location={L"D:\\A\\Project\\src",L"src"};reconcile_rules();verify();
            expect(by_id(live.list(),x).at("rule_id")=="source","navigation reselects directory rule");
        }
        {
            ShellTabTracker live("native-identity",Config{},assume_running);auto first=observation(201);first.native=NativeTab{};first.native->browser_window=first.browser_window;first.native->guid={0x12345678,0x1234,0x1234,{1,2,3,4,5,6,7,8}};
            live.reconcile({{first},true,{}});auto id=live.list()[0].at("tab_id").get<std::string>();live.set_depth(id,3);
            auto replacement=observation(201);replacement.native=first.native;live.reconcile({{replacement},true,{}});
            expect(live.list().size()==1&&live.list()[0].at("tab_id")==id&&live.list()[0].at("override")==3,"COM/native observation transition preserves GUID identity and override");
        }
        {
            ShellTabTracker live("title-test", Config{}, assume_running);
            auto first = observation(201), second = observation(202);
            first.native = NativeTab{}; first.native->browser_window = first.browser_window;
            second.native = NativeTab{}; second.native->browser_window = second.browser_window;
            live.reconcile({{first, second}, true, {}});
            auto ids = live.list(); auto x = ids[0]["tab_id"].get<std::string>(), y = ids[1]["tab_id"].get<std::string>();
            live.set_depth(x, 1); live.set_depth(y, 3);
            std::map<uintptr_t, std::optional<int>> received;
            bool fail = false; int restores = 0;
            ShellTabTracker::TitleTransport bridge = [&](const NativeTab& tab, BridgeAction action, const RuntimePolicy& display) -> BridgeResult {
                if (action == BridgeAction::restore) { ++restores; return {S_OK, L"C"}; }
                if (fail && tab.browser_window == second.browser_window) throw Error(5, "simulated timeout");
                received[reinterpret_cast<uintptr_t>(tab.browser_window)] = display.resolve(std::nullopt).display.depth;
                return {S_OK, L"confirmed title"};
            };
            expect(live.synchronize_titles(true, bridge)["complete"].get<bool>(), "all bridge ACKs succeed");
            expect(received[201] == 1 && received[202] == 3, "bridge receives independent tab depths");
            expect(by_id(live.list(), x)["title_applied"].get<bool>(), "ACK marks current render applied");
            live.set_depth(x, 2);
            expect(!by_id(live.list(), x)["title_applied"].get<bool>(), "policy change invalidates old ACK");
            fail = true;
            expect(live.synchronize_titles(true, bridge)["failed_count"].get<int>() == 1, "partial failure reported");
            expect(by_id(live.list(), x)["title_applied"].get<bool>() && !by_id(live.list(), y)["title_applied"].get<bool>(), "failure isolated to target");
            fail = false;
            expect(live.synchronize_titles(true, bridge)["runtime_applied"].get<bool>(), "lease retry recovers");
            auto stopped = live.synchronize_titles(false, bridge);
            expect(stopped["complete"].get<bool>() && restores == 2, "stop restores all controlled tabs");
            expect(!by_id(live.list(), x)["title_applied"].get<bool>(), "stop clears applied state");
            live.synchronize_titles(false, bridge);
            expect(restores == 2, "repeated stop is idempotent");
        }
        {
            // Startup may find many tabs; a persistent Shell collection race after
            // Explorer restart must not retain dead process lifetimes indefinitely.
            using Key = std::pair<DWORD, uint64_t>;
            std::map<Key, ProcessLifetime> lifetimes{{{42,1000},ProcessLifetime::running},{{84,2000},ProcessLifetime::running},{{84,3000},ProcessLifetime::running}};
            int probes = 0, requests = 0;
            ShellTabTracker live("restart-test", Config{}, [&](DWORD pid, uint64_t started) {
                ++probes; return lifetimes.at({pid,started});
            });
            ShellTabScan initial;
            ShellTabScan partial; partial.complete = false; partial.warnings = {"persistent COM collection race"};
            for (uintptr_t i=0; i<256; ++i) {
                auto tab = observation(1000+i);
                if (i>=128) {tab.process_id=84;tab.process_started=2000;partial.tabs.push_back(tab);}
                tab.native = NativeTab{};tab.native->window=tab.window;tab.native->browser_window=tab.browser_window;
                tab.native->process_id=tab.process_id;tab.native->process_started=tab.process_started;
                initial.tabs.push_back(tab);
                if (i>=128) partial.tabs.back()=tab;
            }
            live.reconcile(initial);
            expect(live.list().size()==256,"startup discovers all already-open tabs");
            expect(probes==2,"process checks cached across many tabs in same process");
            std::string dead_id,survivor_id;
            for (const auto& tab:live.list()) {
                if (tab.at("browser_window")==1000) dead_id=tab.at("tab_id").get<std::string>();
                if (tab.at("browser_window")==1128) survivor_id=tab.at("tab_id").get<std::string>();
            }
            live.set_depth(dead_id,1);live.set_depth(survivor_id,3);
            ShellTabTracker::TitleTransport bridge=[&](const NativeTab& tab,BridgeAction action,const RuntimePolicy&) {
                ++requests;
                expect(lifetimes.at({tab.process_id,tab.process_started})!=ProcessLifetime::ended,"never address retired process lifetime");
                expect(action==BridgeAction::apply,"cleanup does not restore dead or reused target");
                return BridgeResult{S_OK,L"confirmed"};
            };
            expect(live.synchronize_titles(true,bridge).at("applied_count")==256,"startup applies settings to every existing tab");
            lifetimes[{42,1000}]=ProcessLifetime::unknown;
            for (int i=0;i<3;++i) live.reconcile(partial);
            expect(live.list().size()==256,"uncertain process query never retires tabs during repeated partial scans");
            auto missing=by_id(live.list(),dead_id);
            expect(missing.at("override")==1&&!missing.at("observed").get<bool>()&&!missing.at("title_applied").get<bool>(),"uncertain tab keeps override but is not reported as observed or applied");
            requests=0;live.synchronize_titles(true,bridge);
            expect(requests==128,"missing tabs receive no renewal while surviving tabs continue");
            lifetimes[{42,1000}]=ProcessLifetime::ended;
            probes=0;live.reconcile(partial);
            expect(probes==2&&live.list().size()==128,"dead process retired immediately despite incomplete COM scan");
            expect(by_id(live.list(),survivor_id).at("override")==3,"surviving tab identity and override preserved");
            requests=0;expect(live.synchronize_titles(true,bridge).at("complete").get<bool>()&&requests==128,"retired records no longer cause persistent title errors");
            for (auto& tab:partial.tabs) {tab.process_started=3000;tab.native->process_started=3000;}
            lifetimes[{84,2000}]=ProcessLifetime::ended;
            live.reconcile(partial);
            expect(live.list().size()==128,"PID and HWND reuse retires old lifetime only");
            for (const auto& tab:live.list()) expect(tab.at("override").is_null(),"reused process/window never inherits retired override");
            lifetimes[{84,3000}]=ProcessLifetime::ended;
            live.reconcile({{},false,{"COM unavailable"}});
            expect(live.list().empty(),"all dead records removed even when no Shell observations are available");
        }
        std::cout << checks << " tracker checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
