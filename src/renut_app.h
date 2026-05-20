#pragma once
#include <rex/rex_app.h>
#include "renut_engine/overlays/fps_overlay_dialog.h"
#include "renut_engine/renut_logging.h"
#include "renut_engine/overlays/renut_logging_overlay.h"
#include "renut_engine/overlays/path_setup_wizard.h"
#include <functional>
#include <string>

class RenutApp : public rex::ReXApp {
public:
    RenutApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
        : rex::ReXApp(ctx, name, info), app_name_(name) {}

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "renut", PPCImageConfig));
    }

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        drawer->AddDialog(new FpsOverlayDialog(drawer));
        drawer->AddDialog(new RenuLogOverlayDialog(drawer));
        path_wizard_ = new PathSetupWizard(drawer);
        drawer->AddDialog(path_wizard_);
    }

    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        path_wizard_->Init(app_name_, defaults, [resume](rex::PathConfig resolved) {
            resume(resolved);
            });
        return std::nullopt;
    }

private:
    std::string      app_name_;
    PathSetupWizard* path_wizard_ = nullptr;
};