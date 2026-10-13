#include "mod/MyMod.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>

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

// ---------------------------------------------------------------------------
// Teclas em jogo:
//   R      liga/desliga o tracer
//   B      troca a origem da escala horizontal (aspecto da tela  <->  matriz P00)
//   [  ]   diminui / aumenta a escala geral (5% por toque)
//   ,  .   diminui / aumenta so a escala horizontal (5% por toque)
// ---------------------------------------------------------------------------

constexpr int kKeyScaleDown  = 219; // [
constexpr int kKeyScaleUp    = 221; // ]
constexpr int kKeyScaleXDown = 188; // ,
constexpr int kKeyScaleXUp   = 190; // .

bool  gActive       = false;
bool  gTracerOn     = false;
bool  gLogNextFrame = false;
bool  gUseP00       = false; // false: escala X = P11 / aspecto da tela
float gScale        = 1.0f;
float gScaleX       = 1.0f;
bool  gSeenHud      = false;
int   gPassesNoHud  = 0;
std::chrono::steady_clock::time_point gLastDraw{};

struct Vec3f {
    float x, y, z;
};

float dot3(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Projeta um ponto do mundo para coordenadas da GUI. false = atras da camera.
bool worldToScreen(ClientInstance& ci, Vec3f p, float guiW, float guiH, float& sx, float& sy) {
    auto& cam = ci.getCamera();

    Vec3f pos{cam.mPosition->x, cam.mPosition->y, cam.mPosition->z};
    Vec3f fwd{cam.mForward->x, cam.mForward->y, cam.mForward->z};
    Vec3f rgt{cam.mRight->x, cam.mRight->y, cam.mRight->z};
    Vec3f up{cam.mUp->x, cam.mUp->y, cam.mUp->z};

    Vec3f d{p.x - pos.x, p.y - pos.y, p.z - pos.z};
    float z = dot3(d, fwd);
    if (z < 0.05f) {
        return false;
    }
    float x = dot3(d, rgt);
    float y = dot3(d, up);

    auto  proj   = cam.getProjectionMatrix();
    float p00    = std::abs(proj[0][0]);
    float p11    = std::abs(proj[1][1]);
    float aspect = guiW / guiH;

    float kx = gUseP00 ? p00 : p11 / aspect; // fator horizontal
    float ky = p11;                          // fator vertical

    float ndcX = gScale * gScaleX * x * kx / z;
    float ndcY = gScale * y * ky / z;

    sx = (ndcX * 0.5f + 0.5f) * guiW;
    sy = (0.5f - ndcY * 0.5f) * guiH;
    return true;
}

// Recorta o segmento no retangulo [0,W]x[0,H] (Liang-Barsky).
bool clipLine(float& x0, float& y0, float& x1, float& y1, float W, float H) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float t0 = 0.0f;
    float t1 = 1.0f;

    auto clip = [&](float p, float q) {
        if (std::abs(p) < 1e-9f) {
            return q >= 0.0f;
        }
        float r = q / p;
        if (p < 0.0f) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };

    if (clip(-dx, x0) && clip(dx, W - x0) && clip(-dy, y0) && clip(dy, H - y0)) {
        float nx1 = x0 + t1 * dx;
        float ny1 = y0 + t1 * dy;
        float nx0 = x0 + t0 * dx;
        float ny0 = y0 + t0 * dy;
        x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
        return true;
    }
    return false;
}

void fillRect(MinecraftUIRenderContext& ctx, float xa, float xb, float ya, float yb, mce::Color const& color) {
    RectangleArea r;
    r._x0 = xa;
    r._x1 = xb;
    r._y0 = ya;
    r._y1 = yb;
    ctx.fillRectangle(r, color, 1.0f);
}

// Linha fina de verdade: percorre o eixo maior em passos de 1 pixel fisico e junta os
// passos que caem na mesma linha/coluna de pixels em um unico retangulo.
// px = tamanho de 1 pixel fisico em unidades da GUI.
void drawLine(
    MinecraftUIRenderContext& ctx,
    float x0, float y0, float x1, float y1,
    float guiW, float guiH,
    float px, float thickPx,
    mce::Color const& color
) {
    if (!clipLine(x0, y0, x1, y1, guiW, guiH)) {
        return;
    }

    float dx         = x1 - x0;
    float dy         = y1 - y0;
    bool  horizontal = std::abs(dx) >= std::abs(dy);

    float majorLen = horizontal ? std::abs(dx) : std::abs(dy);
    int   n        = std::clamp(static_cast<int>(std::ceil(majorLen / px)), 1, 4000);
    float thick    = thickPx * px;

    auto snap = [&](float v) { return std::floor(v / px) * px; };

    float runMinor = 0.0f;
    float runStart = 0.0f;
    float runEnd   = 0.0f;

    auto emit = [&]() {
        float a = std::min(runStart, runEnd);
        float b = std::max(runStart, runEnd) + px;
        if (horizontal) {
            fillRect(ctx, a, b, runMinor, runMinor + thick, color);
        } else {
            fillRect(ctx, runMinor, runMinor + thick, a, b, color);
        }
    };

    for (int i = 0; i <= n; ++i) {
        float t     = static_cast<float>(i) / static_cast<float>(n);
        float major = horizontal ? (x0 + dx * t) : (y0 + dy * t);
        float minor = snap(horizontal ? (y0 + dy * t) : (x0 + dx * t));

        if (i == 0) {
            runMinor = minor;
            runStart = major;
            runEnd   = major;
        } else if (minor == runMinor) {
            runEnd = major;
        } else {
            emit();
            runMinor = minor;
            runStart = major;
            runEnd   = major;
        }
    }
    emit();
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

    auto  gui  = ci.getGuiData();
    float guiW = gui->mScreenSizeData->clientUIScreenSize->x;
    float guiH = gui->mScreenSizeData->clientUIScreenSize->y;
    if (guiW <= 0.0f || guiH <= 0.0f) {
        return;
    }

    // Quantos pixels fisicos tem cada unidade da GUI.
    float pxPerUnit = gui->mScreenSizeData->clientScreenSize->x / guiW;
    if (pxPerUnit < 0.5f || pxPerUnit > 16.0f) {
        pxPerUnit = 3.0f;
    }
    float px = 1.0f / pxPerUnit;

    Level&     level = local->getLevel();
    mce::Color red(1.0f, 0.1f, 0.1f, 1.0f);

    float cx = guiW * 0.5f;
    float cy = guiH * 0.5f;

    bool  log    = gLogNextFrame;
    int   found  = 0;
    auto& logger = MyMod::getInstance().getSelf().getLogger();

    Actor* localActor = static_cast<Actor*>(local);

    for (Actor* a : level.getRuntimeActorList()) {
        if (!a || a == localActor || !a->isPlayer() || !a->isAlive()) {
            continue;
        }
        ++found;

        // Tronco = centro da caixa de colisao do jogador.
        auto const& box = a->getAABB();
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
                logger.info("alvo {}: tela ({:.1f},{:.1f}) de {:.0f}x{:.0f}", found, sx, sy, guiW, guiH);
            } else {
                logger.info("alvo {}: atras da camera", found);
            }
        }
        if (ok) {
            drawLine(ctx, cx, cy, sx, sy, guiW, guiH, px, 2.0f, red);
        }
    }

    if (log) {
        auto  proj = ci.getCamera().getProjectionMatrix();
        float p00  = proj[0][0];
        float p11  = proj[1][1];
        logger.info(
            "diagnostico: jogadores={} P00={:.3f} P11={:.3f} P11/P00={:.3f} aspecto_tela={:.3f} usaP00={} escala={:.2f} escalaX={:.2f} px/unidade={:.2f}",
            found, p00, p11, p00 != 0.0f ? p11 / p00 : 0.0f, guiW / guiH, gUseP00, gScale, gScaleX, pxPerUnit
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
    getSelf().getLogger().info("Meu Tracer: enable - R liga/desliga; B, [ ] , . calibram");
    gActive = true;

    auto& bus = ll::event::EventBus::getInstance();

    bus.emplaceListener<ll::event::KeyInputEvent>([this](ll::event::KeyInputEvent& event) {
        int code = event.keyCode();
        if (!gActive || code < 0 || code > 255) {
            return;
        }

        static bool held[256] = {};
        if (!event.isDown()) {
            held[code] = false;
            return;
        }
        if (held[code]) {
            return; // tecla segurada: ignora repeticao
        }
        held[code] = true;

        auto& logger = getSelf().getLogger();
        switch (code) {
        case 'R':
            gTracerOn     = !gTracerOn;
            gLogNextFrame = gTracerOn;
            logger.info("Tracer {}", gTracerOn ? "LIGADO" : "DESLIGADO");
            break;
        case 'B':
            gUseP00       = !gUseP00;
            gLogNextFrame = true;
            logger.info("Escala horizontal vem de: {}", gUseP00 ? "P00 (matriz)" : "P11 / aspecto da tela");
            break;
        case kKeyScaleDown:
            gScale        = std::max(0.2f, gScale * 0.95f);
            gLogNextFrame = true;
            logger.info("Escala geral = {:.3f}", gScale);
            break;
        case kKeyScaleUp:
            gScale        = std::min(5.0f, gScale * 1.05f);
            gLogNextFrame = true;
            logger.info("Escala geral = {:.3f}", gScale);
            break;
        case kKeyScaleXDown:
            gScaleX       = std::max(0.2f, gScaleX * 0.95f);
            gLogNextFrame = true;
            logger.info("Escala horizontal = {:.3f}", gScaleX);
            break;
        case kKeyScaleXUp:
            gScaleX       = std::min(5.0f, gScaleX * 1.05f);
            gLogNextFrame = true;
            logger.info("Escala horizontal = {:.3f}", gScaleX);
            break;
        default:
            break;
        }
    });

    bus.emplaceListener<ll::event::AfterUIRenderEvent>([this](ll::event::AfterUIRenderEvent& event) {
        if (!gActive || !gTracerOn) {
            return;
        }

        // O evento dispara uma vez por tela. Desenhamos so na tela do HUD.
        std::string name = event.screenView().getScreenName();

        static std::set<std::string> seen;
        if (seen.size() < 12 && seen.insert(name).second) {
            getSelf().getLogger().info("tela de UI vista: {}", name);
        }

        bool isHud = name.find("hud") != std::string::npos;
        bool draw  = false;
        if (isHud) {
            gSeenHud = true;
        }
        if (gSeenHud) {
            draw = isHud;
        } else if (++gPassesNoHud > 300) {
            // Nenhuma tela com "hud" no nome: desenha uma vez por quadro em qualquer tela.
            auto now = std::chrono::steady_clock::now();
            if (now - gLastDraw >= std::chrono::milliseconds(4)) {
                gLastDraw = now;
                draw      = true;
            }
        }

        if (draw) {
            drawTracers(event.uiRenderContext());
        }
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