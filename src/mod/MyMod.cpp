#include "mod/MyMod.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
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

// ---------------------------------------------------------------------------
// Controles em jogo:
//   R = liga/desliga o tracer
//   T = troca o modo de projecao (0..4)  <- use ate a linha cair em cima do jogador
//   Y = inverte vertical   U = inverte horizontal
// ---------------------------------------------------------------------------

enum Mode { MatCol = 0, MatRow = 1, LookCamFov = 2, Look70 = 3, Basis = 4, ModeCount = 5 };

constexpr char const* kModeNames[ModeCount] = {
    "0: matrizes view/proj (coluna)",
    "1: matrizes view/proj (linha)",
    "2: olhar do jogador + FOV da projecao",
    "3: olhar do jogador + FOV 70",
    "4: base da camera (versao antiga)"
};

bool  gActive         = false;
bool  gTracerOn       = false;
bool  gLogNextFrame   = false;
int   gMode           = 0;
float gFlipX          = 1.0f;
float gFlipY          = 1.0f;
bool  gSeenHud        = false;
int   gPassesNoHud    = 0;
std::chrono::steady_clock::time_point gLastDraw{};

struct Vec3f {
    float x, y, z;
};
struct Vec4f {
    float x, y, z, w;
};

// glm guarda por coluna: m[coluna][linha].  r = m * v
Vec4f mulColumn(glm::mat4 const& m, Vec4f v) {
    return {
        m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
        m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
        m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
        m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w
    };
}

// convencao de DirectX: vetor-linha.  r = v * m
Vec4f mulRow(glm::mat4 const& m, Vec4f v) {
    return {
        v.x * m[0][0] + v.y * m[0][1] + v.z * m[0][2] + v.w * m[0][3],
        v.x * m[1][0] + v.y * m[1][1] + v.z * m[1][2] + v.w * m[1][3],
        v.x * m[2][0] + v.y * m[2][1] + v.z * m[2][2] + v.w * m[2][3],
        v.x * m[3][0] + v.y * m[3][1] + v.z * m[3][2] + v.w * m[3][3]
    };
}

float dot3(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3f norm3(Vec3f a) {
    float l = std::sqrt(dot3(a, a));
    if (l < 1e-6f) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {a.x / l, a.y / l, a.z / l};
}

Vec3f cross3(Vec3f a, Vec3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool toScreenFromNdc(float ndcX, float ndcY, float guiW, float guiH, float& sx, float& sy) {
    sx = (gFlipX * ndcX * 0.5f + 0.5f) * guiW;
    sy = (0.5f - gFlipY * ndcY * 0.5f) * guiH;
    return true;
}

// Projeta um ponto do mundo para coordenadas da GUI. false = atras da camera.
bool worldToScreen(ClientInstance& ci, LocalPlayer& lp, Vec3f p, float guiW, float guiH, float& sx, float& sy) {
    auto& cam = ci.getCamera();

    switch (gMode) {
    case MatCol:
    case MatRow: {
        glm::mat4 view = *cam.viewMatrixStack->top()._m;
        glm::mat4 proj = *cam.projectionMatrixStack->top()._m;
        Vec4f     v{p.x, p.y, p.z, 1.0f};
        Vec4f     clip;
        if (gMode == MatCol) {
            clip = mulColumn(proj, mulColumn(view, v));
        } else {
            clip = mulRow(proj, mulRow(view, v)); // (v * view) * proj
        }
        if (clip.w < 0.05f) {
            return false;
        }
        return toScreenFromNdc(clip.x / clip.w, clip.y / clip.w, guiW, guiH, sx, sy);
    }
    case LookCamFov:
    case Look70: {
        Vec3 eyeV = lp.getEyePos();
        Vec3 fV   = lp.getViewVector(1.0f);
        Vec3f eye{eyeV.x, eyeV.y, eyeV.z};
        Vec3f f = norm3({fV.x, fV.y, fV.z});
        Vec3f right = norm3({-f.z, 0.0f, f.x});
        Vec3f up    = cross3(right, f);

        Vec3f d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
        float z = dot3(d, f);
        if (z < 0.05f) {
            return false;
        }
        float x = dot3(d, right);
        float y = dot3(d, up);

        float t = std::tan(70.0f * 3.14159265f / 180.0f * 0.5f); // modo 3: FOV 70
        if (gMode == LookCamFov) {
            float p11 = std::abs(cam.getProjectionMatrix()[1][1]); // = 1 / tan(fov/2)
            if (p11 < 1e-4f) {
                return false;
            }
            t = 1.0f / p11;
        }
        float aspect = guiW / guiH;
        if (t < 1e-4f) {
            return false;
        }
        return toScreenFromNdc(x / (z * t * aspect), y / (z * t), guiW, guiH, sx, sy);
    }
    default: { // Basis: versao antiga
        Vec3f pos{cam.mPosition->x, cam.mPosition->y, cam.mPosition->z};
        Vec3f fwd{cam.mForward->x, cam.mForward->y, cam.mForward->z};
        Vec3f rgt{cam.mRight->x, cam.mRight->y, cam.mRight->z};
        Vec3f up{cam.mUp->x, cam.mUp->y, cam.mUp->z};
        Vec3f d{p.x - pos.x, p.y - pos.y, p.z - pos.z};
        float z = dot3(d, fwd);
        if (z < 0.05f) {
            return false;
        }
        float x    = dot3(d, rgt);
        float y    = dot3(d, up);
        auto  proj = cam.getProjectionMatrix();
        return toScreenFromNdc(x * std::abs(proj[0][0]) / z, y * std::abs(proj[1][1]) / z, guiW, guiH, sx, sy);
    }
    }
    return false;
}

// O desenhista de UI nao tem "linha": desenhamos uma fila de quadradinhos.
void drawLine(MinecraftUIRenderContext& ctx, float x0, float y0, float x1, float y1, mce::Color const& color) {
    float dx    = x1 - x0;
    float dy    = y1 - y0;
    float len   = std::sqrt(dx * dx + dy * dy);
    int   steps = std::clamp(static_cast<int>(len / 1.5f), 1, 250);
    constexpr float h = 0.75f;

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

    auto  gui  = ci.getGuiData();
    float guiW = gui->mScreenSizeData->clientUIScreenSize->x;
    float guiH = gui->mScreenSizeData->clientUIScreenSize->y;
    if (guiW <= 0.0f || guiH <= 0.0f) {
        return;
    }

    Level& level = local->getLevel();
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
        bool  ok = worldToScreen(ci, *local, torso, guiW, guiH, sx, sy);
        if (log) {
            if (ok) {
                logger.info(
                    "alvo {}: mundo ({:.1f},{:.1f},{:.1f}) -> tela ({:.1f},{:.1f}) de {:.0f}x{:.0f}",
                    found, torso.x, torso.y, torso.z, sx, sy, guiW, guiH
                );
            } else {
                logger.info("alvo {}: atras da camera", found);
            }
        }
        if (ok) {
            drawLine(ctx, cx, cy, sx, sy, red);
        }
    }

    if (log) {
        auto& cam = ci.getCamera();
        logger.info(
            "diagnostico: modo={} flipX={} flipY={} jogadores={} camPos=({:.1f},{:.1f},{:.1f}) P00={:.3f} P11={:.3f}",
            kModeNames[gMode], gFlipX, gFlipY, found, cam.mPosition->x, cam.mPosition->y, cam.mPosition->z,
            cam.getProjectionMatrix()[0][0], cam.getProjectionMatrix()[1][1]
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
    getSelf().getLogger().info("Meu Tracer: enable - R liga/desliga, T troca modo, Y/U invertem");
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
        case 'T':
            gMode         = (gMode + 1) % ModeCount;
            gLogNextFrame = true;
            logger.info("Modo de projecao {}", kModeNames[gMode]);
            break;
        case 'Y':
            gFlipY        = -gFlipY;
            gLogNextFrame = true;
            logger.info("Vertical invertido: {}", gFlipY < 0.0f ? "sim" : "nao");
            break;
        case 'U':
            gFlipX        = -gFlipX;
            gLogNextFrame = true;
            logger.info("Horizontal invertido: {}", gFlipX < 0.0f ? "sim" : "nao");
            break;
        default:
            break;
        }
    });

    bus.emplaceListener<ll::event::AfterUIRenderEvent>([this](ll::event::AfterUIRenderEvent& event) {
        if (!gActive || !gTracerOn) {
            return;
        }

        // O evento dispara uma vez por tela. Desenhamos so na tela do HUD,
        // senao a linha some e volta conforme outras telas pintam por cima.
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
            // Nao achamos nenhuma tela com "hud" no nome: desenha uma vez por quadro em qualquer tela.
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