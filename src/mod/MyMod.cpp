#include "mod/MyMod.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h" // este header nao tem pragma once: inclua so aqui
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace my_mod {

namespace {

bool gActive      = false; // mod habilitado
bool gTracerOn    = false; // liga/desliga com R
bool gLogNextFrame = false; // imprime diagnostico no primeiro frame depois de ligar

// Se a linha aparecer espelhada, troque o sinal correspondente para -1.0f.
constexpr float kFlipX = 1.0f;
constexpr float kFlipY = 1.0f;

struct Vec3f {
    float x, y, z;
};

// Projeta um ponto do mundo para coordenadas da GUI (mesma unidade do fillRectangle).
// Retorna false se o ponto estiver atras da camera.
bool worldToScreen(ClientInstance& ci, Vec3f p, float guiW, float guiH, float& sx, float& sy) {
    auto& cam = ci.getCamera();

    Vec3f pos{cam.mPosition->x, cam.mPosition->y, cam.mPosition->z};
    Vec3f fwd{cam.mForward->x, cam.mForward->y, cam.mForward->z};
    Vec3f rgt{cam.mRight->x, cam.mRight->y, cam.mRight->z};
    Vec3f up{cam.mUp->x, cam.mUp->y, cam.mUp->z};

    Vec3f d{p.x - pos.x, p.y - pos.y, p.z - pos.z};
    float z = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
    if (z < 0.05f) {
        return false;
    }
    float x = d.x * rgt.x + d.y * rgt.y + d.z * rgt.z;
    float y = d.x * up.x + d.y * up.y + d.z * up.z;

    auto proj = cam.getProjectionMatrix();
    float px  = std::abs(proj[0][0]); // 1 / (aspect * tan(fov/2))
    float py  = std::abs(proj[1][1]); // 1 / tan(fov/2)

    float ndcX = kFlipX * x * px / z;
    float ndcY = kFlipY * y * py / z;

    sx = (ndcX * 0.5f + 0.5f) * guiW;
    sy = (0.5f - ndcY * 0.5f) * guiH;
    return true;
}

// O desenhista de UI nao tem "linha": desenhamos uma fila de quadradinhos.
void drawLine(MinecraftUIRenderContext& ctx, float x0, float y0, float x1, float y1, mce::Color const& color) {
    float dx    = x1 - x0;
    float dy    = y1 - y0;
    float len   = std::sqrt(dx * dx + dy * dy);
    int   steps = std::clamp(static_cast<int>(len), 1, 800);
    constexpr float h = 0.6f; // metade da espessura, em unidades de GUI

    for (int i = 0; i <= steps; ++i) {
        float t  = static_cast<float>(i) / static_cast<float>(steps);
        float cx = x0 + dx * t;
        float cy = y0 + dy * t;

        RectangleArea r;
        r._x0 = cx - h;
        r._x1 = cx + h;
        r._y0 = cy - h;
        r._y1 = cy + h;
        ctx.fillRectangle(r, color, 1.0f);
    }
}

void drawTracers(MinecraftUIRenderContext& ctx) {
    auto ciRef = ll::service::getClientInstance();
    if (!ciRef) {
        return;
    }
    ClientInstance& ci = *ciRef;

    LocalPlayer* local = ci.getLocalPlayer();
    if (!local) {
        return;
    }

    auto gui  = ci.getGuiData();
    float guiW = gui->mScreenSizeData->clientUIScreenSize->x;
    float guiH = gui->mScreenSizeData->clientUIScreenSize->y;
    if (guiW <= 0.0f || guiH <= 0.0f) {
        return;
    }

    Level& level = local->getLevel();
    mce::Color red(1.0f, 0.1f, 0.1f, 1.0f);

    float cx = guiW * 0.5f;
    float cy = guiH * 0.5f;

    bool  log   = gLogNextFrame;
    int   found = 0;
    auto& logger = MyMod::getInstance().getSelf().getLogger();

    level.forEachPlayer(std::function<bool(Player&)>{[&](Player& p) {
        if (&p == static_cast<Player*>(local) || !p.isAlive()) {
            return true;
        }
        ++found;

        // Tronco = centro da caixa de colisao do jogador.
        auto const& box = p.getAABB();
        Vec3f torso{
            (box.min.x + box.max.x) * 0.5f,
            (box.min.y + box.max.y) * 0.5f,
            (box.min.z + box.max.z) * 0.5f
        };

        float sx = 0.0f;
        float sy = 0.0f;
        bool  ok = worldToScreen(ci, torso, guiW, guiH, sx, sy);
        if (log) {
            if (ok) {
                logger.info("alvo {}: tela ({:.1f}, {:.1f}) de {:.0f}x{:.0f}", found, sx, sy, guiW, guiH);
            } else {
                logger.info("alvo {}: atras da camera", found);
            }
        }
        if (ok) {
            drawLine(ctx, cx, cy, sx, sy, red);
        }
        return true;
    }});

    if (log) {
        auto& cam  = ci.getCamera();
        auto  proj = cam.getProjectionMatrix();
        logger.info(
            "diagnostico: jogadores={} atores={} camPos=({:.1f},{:.1f},{:.1f}) fwd=({:.2f},{:.2f},{:.2f}) P00={:.3f} P11={:.3f}",
            found,
            level.getRuntimeActorList().size(),
            cam.mPosition->x,
            cam.mPosition->y,
            cam.mPosition->z,
            cam.mForward->x,
            cam.mForward->y,
            cam.mForward->z,
            proj[0][0],
            proj[1][1]
        );
        gLogNextFrame = false;
    }
}

} // namespace

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() {
    getSelf().getLogger().info("Meu Tracer: load");
    return true;
}

bool MyMod::enable() {
    getSelf().getLogger().info("Meu Tracer: enable - aperte R para ligar/desligar");
    gActive = true;

    auto& bus = ll::event::EventBus::getInstance();

    bus.emplaceListener<ll::event::KeyInputEvent>([this](ll::event::KeyInputEvent& event) {
        if (!gActive || event.keyCode() != 'R') {
            return;
        }

        // Evita alternar varias vezes enquanto a tecla fica segurada.
        static bool released = true;
        if (event.isDown()) {
            if (released) {
                released      = false;
                gTracerOn     = !gTracerOn;
                gLogNextFrame = gTracerOn;
                getSelf().getLogger().info("Tracer {}", gTracerOn ? "LIGADO" : "DESLIGADO");
            }
        } else {
            released = true;
        }
    });

    bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& event) {
        if (!gActive || !gTracerOn) {
            return;
        }
        drawTracers(event.uiRenderContext());
    });

    return true;
}

bool MyMod::disable() {
    getSelf().getLogger().info("Meu Tracer: disable");
    gActive   = false;
    gTracerOn = false;
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());