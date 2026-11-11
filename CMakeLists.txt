cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK non impostato (export VITASDK=$HOME/vitasdk)")
endif()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")

project(vampiresurvivors C)
set(CMAKE_C_STANDARD 99)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Vampire Survivors")
set(VITA_TITLEID  "VSPH00001")
set(VITA_VERSION  "01.20")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -fno-lto")

file(GLOB SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_executable(${PROJECT_NAME} ${SOURCES} src/third_party/cJSON.c)
target_include_directories(${PROJECT_NAME} PRIVATE src)

# Come FX-Ball: raylib (porting 5.5) -> SDL2 -> vitaGL -> stub SCE.
target_link_libraries(${PROJECT_NAME}
    raylib SDL2 vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon
    SceLibKernel_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub
    SceAudio_stub SceAudioIn_stub SceAvPlayer_stub SceSysmodule_stub SceGxm_stub SceCommonDialog_stub
    SceIme_stub SceAppMgr_stub SceAppUtil_stub SceFiber_stub ScePower_stub SceHid_stub
    ScePvf_stub ScePgf_stub SceIofilemgr_stub SceProcessmgr_stub SceRtc_stub SceFios2_stub
    SceVshBridge_stub ScePromoterUtil_stub SceKernelDmacMgr_stub SceNet_stub SceNpTrophy_stub SceNetCtl_stub
    m pthread stdc++ z
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)

# Niente asset nel VPK: i dati del gioco vanno in ux0:data/VampireSurvivors/ (tools/prepare.sh).
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/trophy/VSPH00001_00/TROPHY.TRP  sce_sys/trophy/VSPH00001_00/TROPHY.TRP
    FILE sce_sys/trophy/VSPH00001_01/TROPHY.TRP  sce_sys/trophy/VSPH00001_01/TROPHY.TRP
    FILE assets/home.mp4                         assets/home.mp4
    FILE assets/intro.mp4                        assets/intro.mp4
    FILE assets/intro.ogg                        assets/intro.ogg
    FILE assets/relics/glasses3d.png             assets/relics/glasses3d.png
    FILE assets/relics/vinyl0.png                assets/relics/vinyl0.png
    FILE assets/relics/vinyl1.png                assets/relics/vinyl1.png
    FILE assets/relics/vinyl2.png                assets/relics/vinyl2.png
    FILE assets/relics/vinyl3.png                assets/relics/vinyl3.png
    FILE assets/relics/vinyl4.png                assets/relics/vinyl4.png
    FILE assets/relics/vinyl5.png                assets/relics/vinyl5.png
    FILE assets/relics/vinyl6.png                assets/relics/vinyl6.png
    FILE assets/relics/vinyl7.png                assets/relics/vinyl7.png
    FILE assets/glyphs/button_ps4_cross_0.png    assets/glyphs/button_ps4_cross_0.png
    FILE assets/glyphs/button_ps4_circle_0.png   assets/glyphs/button_ps4_circle_0.png
    FILE assets/glyphs/button_ps4_square_0.png   assets/glyphs/button_ps4_square_0.png
    FILE assets/glyphs/button_ps4_triangle_0.png assets/glyphs/button_ps4_triangle_0.png
    FILE assets/glyphs/button_ps4_start.png      assets/glyphs/button_ps4_start.png
    FILE assets/glyphs/button_psv_L.png          assets/glyphs/button_psv_L.png
    FILE assets/glyphs/button_psv_R.png          assets/glyphs/button_psv_R.png
    FILE sce_sys/icon0.png                       sce_sys/icon0.png
    FILE sce_sys/pic0.png                        sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg.png        sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png   sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml  sce_sys/livearea/contents/template.xml
)
