#include "mod/MyMod.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <unordered_map>

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
// ESP simples: desenha uma caixa ao redor de cada jogador.
//
// Teclas em jogo:
//   R      liga/desliga o ESP
//   B      troca a origem da escala horizontal (aspecto da tela  <->  matriz P00)
//   [  ]   diminui / aumenta a escala geral (5% por toque)
//   ,  .   diminui / aumenta so a escala horizontal (5% por toque)
// ---------------------------------------------------------------------------

constexpr int kKeyScaleDown  = 219; // [
constexpr int kKeyScaleUp    = 221; // ]
constexpr int kKeyScaleXDown = 188; // ,
constexpr int kKeyScaleXUp   = 190; // .

bool  gActive       = false;
bool  gEspOn        = false;
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

// Dados da camera lidos uma vez por quadro.
struct CamData {
    Vec3f pos, fwd, rgt, up;
    float kx, ky;       // fatores de escala horizontal e vertical
    float guiW, guiH;
};

CamData readCamera(ClientInstance& ci, float guiW, float guiH) {
    auto& cam = ci.getCamera();

    CamData c;
    c.pos = {cam.mPosition->x, cam.mPosition->y, cam.mPosition->z};
    c.fwd = {cam.mForward->x, cam.mForward->y, cam.mForward->z};
    c.rgt = {cam.mRight->x, cam.mRight->y, cam.mRight->z};
    c.up  = {cam.mUp->x, cam.mUp->y, cam.mUp->z};

    auto  proj   = cam.getProjectionMatrix();
    float p00    = std::abs(proj[0][0]);
    float p11    = std::abs(proj[1][1]);
    float aspect = guiW / guiH;

    c.kx   = gScale * gScaleX * (gUseP00 ? p00 : p11 / aspect);
    c.ky   = gScale * p11;
    c.guiW = guiW;
    c.guiH = guiH;
    return c;
}

// Projeta um ponto do mundo para coordenadas da GUI. false = atras da camera.
bool project(CamData const& c, Vec3f p, float& sx, float& sy) {
    Vec3f d{p.x - c.pos.x, p.y - c.pos.y, p.z - c.pos.z};
    float z = dot3(d, c.fwd);
    if (z < 0.05f) {
        return false;
    }
    float x = dot3(d, c.rgt);
    float y = dot3(d, c.up);

    float ndcX = x * c.kx / z;
    float ndcY = y * c.ky / z;

    sx = (ndcX * 0.5f + 0.5f) * c.guiW;
    sy = (0.5f - ndcY * 0.5f) * c.guiH;
    return true;
}

// Projeta os 8 cantos da caixa de colisao e devolve o retangulo que cobre todos.
bool projectBox(
    CamData const& c,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float& minX, float& minY, float& maxX, float& maxY
) {
    minX = minY = 1e9f;
    maxX = maxY = -1e9f;

    for (int i = 0; i < 8; ++i) {
        Vec3f corner{(i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0};
        float sx = 0.0f;
        float sy = 0.0f;
        if (!project(c, corner, sx, sy)) {
            return false; // algum canto atras da camera: pula este jogador
        }
        minX = std::min(minX, sx);
        maxX = std::max(maxX, sx);
        minY = std::min(minY, sy);
        maxY = std::max(maxY, sy);
    }
    return true;
}

void drawEsp(MinecraftUIRenderContext& ctx) {
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

    CamData    cam = readCamera(ci, guiW, guiH);
    Level&     level = local->getLevel();
    mce::Color red(1.0f, 0.1f, 0.1f, 1.0f);

    bool  log    = gLogNextFrame;
    int   found  = 0;
    auto& logger = MyMod::getInstance().getSelf().getLogger();

    Actor* localActor = static_cast<Actor*>(local);

    for (Actor* a : level.getRuntimeActorList()) {
        if (!a || a == localActor || !a->isPlayer() || !a->isAlive()) {
            continue;
        }
        ++found;

        auto const& box = a->getAABB();

        float minX, minY, maxX, maxY;
        bool  ok = projectBox(
            cam,
            box.min.x, box.min.y, box.min.z,
            box.max.x, box.max.y, box.max.z,
            minX, minY, maxX, maxY
        );

        if (log) {
            if (ok) {
                logger.info("jogador {}: caixa ({:.1f},{:.1f}) - ({:.1f},{:.1f}) de {:.0f}x{:.0f}", found, minX, minY, maxX, maxY, guiW, guiH);
            } else {
                logger.info("jogador {}: atras da camera", found);
            }
        }
        if (!ok) {
            continue;
        }
        if (maxX < 0.0f || minX > guiW || maxY < 0.0f || minY > guiH) {
            continue; // fora da tela
        }

        RectangleArea r;
        r._x0 = minX;
        r._x1 = maxX;
        r._y0 = minY;
        r._y1 = maxY;
        ctx.drawRectangle(r, red, 1.0f, 1);
    }

    if (log) {
        auto proj = ci.getCamera().getProjectionMatrix();
        logger.info(
            "diagnostico: jogadores={} P00={:.3f} P11={:.3f} aspecto_tela={:.3f} usaP00={} escala={:.2f} escalaX={:.2f}",
            found, proj[0][0], proj[1][1], guiW / guiH, gUseP00, gScale, gScaleX
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
    getSelf().getLogger().info("Meu ESP: load");
    return true;
}

bool MyMod::enable() {
    getSelf().getLogger().info("Meu ESP: enable - R liga/desliga; B, [ ] , . calibram");
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
            gEspOn        = !gEspOn;
            gLogNextFrame = gEspOn;
            logger.info("ESP {}", gEspOn ? "LIGADO" : "DESLIGADO");
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
        if (!gActive || !gEspOn) {
            return;
        }

        // O evento dispara uma vez por tela. Desenhamos so na tela do HUD.
        // O nome de cada tela so e lido uma vez (guardado por ponteiro) para nao pesar.
        static std::unordered_map<void const*, bool> hudCache;
        ScreenView& sv = event.screenView();

        bool isHud;
        auto it = hudCache.find(&sv);
        if (it != hudCache.end()) {
            isHud = it->second;
        } else {
            std::string name = sv.getScreenName();
            isHud            = name.find("hud") != std::string::npos;
            if (hudCache.size() > 64) {
                hudCache.clear();
            }
            hudCache[&sv] = isHud;

            static std::set<std::string> seen;
            if (seen.size() < 12 && seen.insert(name).second) {
                getSelf().getLogger().info("tela de UI vista: {}", name);
            }
        }

        bool draw = false;
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
            drawEsp(event.uiRenderContext());
        }
    });

    return true;
}

bool MyMod::disable() {
    getSelf().getLogger().info("Meu ESP: disable");
    gActive = false;
    gEspOn  = false;
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());