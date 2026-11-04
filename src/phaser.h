// Sottoinsieme di Phaser 3.55.2 usato da Vampire Survivors, tradotto in C dai
// sorgenti originali (MIT, photonstorm/phaser, src/time, src/tweens,
// src/animations, src/physics/arcade). Nomi e passi seguono l'originale; dove
// il gioco non usa un ramo (gravita', drag, accelerazione, tilemap) il ramo manca.
#ifndef PHASER_H
#define PHASER_H

#include "vs.h"

typedef struct Sprite Sprite;

// --- Memoria della partita: tutto cio' che una partita alloca si libera in blocco alla successiva
// (Phaser ricarica la scena e lascia il resto al garbage collector) --------------------------------
void *gmem_calloc(size_t n, size_t size);
void *gmem_realloc(void *p, size_t size);
void gmem_free(void *p);
void gmem_reset(void);
size_t gmem_count(void);
typedef void (*Callback)(void *ctx);

// --- Clock / TimerEvent (src/time) ------------------------------------------
typedef struct TimerEvent {
    float delay, elapsed, timeScale;
    double repeatCount;
    int paused, hasDispatched, active, pendingRemoval;
    Callback callback;
    void *ctx;
    unsigned gen;
} TimerEvent;
typedef struct { int idx; unsigned gen; } Timer;  // riferimento sicuro a un evento (gli eventi si riciclano)

Timer time_addEvent(float delay, int loop, int repeat, Callback cb, void *ctx);
void time_removeEvent(Timer t);
TimerEvent *time_get(Timer t);  // NULL se l'evento non esiste piu'
void time_reset(void);
void clock_preUpdate(void);
void clock_update(float delta);

// --- Tween (src/tweens/tween/Tween.js + TweenManager.js, ease Linear) --------
// Solo cio' che il gioco usa: target numerici senza 'from', yoyo, repeat, loop,
// delay, onComplete/onRepeat. 'scale' scrive scaleX e scaleY (t2).
#define TWEEN_DATA 3
enum { TS_PENDING_ADD, TS_INIT, TS_ACTIVE, TS_PAUSED, TS_PENDING_REMOVE, TS_REMOVED };
enum { TD_CREATED, TD_DELAY, TD_PENDING_RENDER, TD_PLAYING_FORWARD, TD_PLAYING_BACKWARD, TD_COMPLETE };
typedef struct {
    float *t, *t2;
    float start, end, endCfg, current, elapsed, delay, duration;
    int state, yoyo;
    double repeat, repeatCounter;
    float (*ease)(float);  // NULL = Linear
    float (*get)(void *o);  // proprieta' calcolata (getter/setter, es. Face.x): t = NULL
    void (*set)(void *o, float v);
    void *o;
} TweenData;
typedef struct Tween {
    TweenData inl[TWEEN_DATA], *data;  // data = inl, o sul heap oltre TWEEN_DATA (targets: array di molti oggetti)
    int totalData, state, pausedState, paused, inAdd, inActive, inPending;
    double loop, loopCounter;
    float timeScale;
    Callback onComplete, onRepeat, onYoyo;  // onYoyo: una volta per ogni TweenData che fa yoyo, come l'originale
    Callback onStart;  // quando il primo dato esce dal delay (TWEEN_START); di nuovo dopo restart
    Callback onUpdate;  // TWEEN_UPDATE: qui una volta per passo attivo (l'originale: per TweenData; uguale con un solo dato)
    int hasStarted;
    void *ctx;
    unsigned gen;
    int used, persist;
} Tween;
typedef struct { int idx; unsigned gen; } TweenRef;
typedef struct { float *t, *t2; float end; float (*get)(void *o); void (*set)(void *o, float v); void *o; } TweenProp;
typedef struct {
    TweenProp props[TWEEN_DATA];
    int nprops;
    float duration, delay;
    int yoyo, repeat, loop;  // repeat/loop: -1 = infinito
    int paused;              // 'paused': true -> resta in attesa finche' non si chiama play/resume
    float (*ease)(float);    // NULL = Linear; Ease_BounceOut, Ease_SineInOut
    Callback onComplete, onRepeat, onYoyo, onStart, onUpdate;
    void *ctx;
    const TweenProp *many;   // al posto di props quando i TweenData sono piu' di TWEEN_DATA
    int nmany;
    int persist;             // il chiamante tiene il riferimento e lo fa ripartire dopo la fine (restart/play): lo slot non si libera
} TweenConfig;
TweenRef tweens_add(TweenConfig c);
float Ease_BounceOut(float v);  // src/math/easing
float Ease_SineInOut(float v);
float Ease_ExpoOut(float v);
float Ease_ExpoIn(float v);
float Ease_QuintOut(float v);
float Ease_BackOut(float v);
float Ease_SineOut(float v);
Tween *tween_get(TweenRef r);
int tweens_used(void);  // VS_PROF: slot occupati su 4096
void tween_restart(TweenRef r);
void tween_stop(TweenRef r);
void tween_remove(TweenRef r);  // scene.tweens.remove
int tween_isPaused(TweenRef r);
void tween_pause(TweenRef r);
void tween_play(TweenRef r);
void tween_resume(TweenRef r);
void tweens_reset(void);
void tweens_preUpdate(void);
void tweens_update(float delta);

// --- Animazioni (src/animations) ---------------------------------------------
#define ANIM_MAX_FRAMES 64
typedef struct {
    const Frame *frames[ANIM_MAX_FRAMES];
    int n, repeat;  // repeat -1 = infinito
    float frameRate, msPerFrame;
} Animation;
typedef struct {
    const Animation *cur;
    int index, isPlaying, paused, wasPlaying, repeatCounter;
    float accumulator, nextTick, timeScale;
} AnimationState;

// --- Arcade Physics: Body (src/physics/arcade/Body.js) ------------------------
typedef struct { int none, up, down, left, right; } Flags;
typedef struct Body {
    Sprite *gameObject;
    int enable, isCircle, allowRotation, immovable, pushable, moves, customSeparateX, customSeparateY, embedded;
    float radius, rotation, preRotation, mass, angle, speed;
    float width, height, sourceWidth, sourceHeight, halfWidth, halfHeight;
    Vector2 offset, position, prev, prevFrame, center, velocity, newVelocity, deltaMax, bounce, maxVelocity;
    struct { float x, y, rotation, scaleX, scaleY, displayOriginX, displayOriginY; } transform;
    float overlapX, overlapY, overlapR;
    Flags checkCollision, touching, wasTouching, blocked;
    float _sx, _sy, _dx, _dy, _tx, _ty;
    int inWorld;
    // setCollideWorldBounds(true, bx, by) + setBoundsRectangle(r) + onWorldBounds: evento 'worldbounds' come callback del corpo
    int collideWorldBounds, hasWorldBounce;
    int noGravity;  // setAllowGravity(false)
    Vector2 worldBounce;
    const Rectangle *customBounds;
    void (*onWorldBounds)(void *ctx);
    void *onWorldBoundsCtx;
} Body;

void body_setCircle(Body *b, float radius, float offsetX, float offsetY);
void body_setSize(Body *b, float width, float height);  // rettangolo centrato sul fotogramma (center = true)
void body_setOffset(Body *b, float x, float y);
void body_updateCenter(Body *b);
static inline float body_right(const Body *b) { return b->position.x + b->width; }
static inline float body_bottom(const Body *b) { return b->position.y + b->height; }

// --- Sprite (Phaser.Physics.Arcade.Sprite, solo i componenti usati) ----------
struct Sprite {
    float x, y, scaleX, scaleY, angle, originX, originY, alpha;
    double depth;  // Number come in JS: ZInGameUI = MAX_SAFE_INTEGER - 1000 deve restare distinto
    int flipX, flipY, visible, active, onDisplayList, scrollFactor0;  // scrollFactor0: setScrollFactor(0), coordinate di schermo
    int blendAdd;  // setBlendMode(ADD)
    unsigned tint;
    int tintFill;
    const Frame *frame;
    float width, height;  // dimensione reale del fotogramma (frame.realWidth)
    AnimationState anims;
    Callback onAnimationComplete;
    void *owner;          // l'oggetto del gioco che possiede lo sprite
    void (*draw)(Sprite *s);  // disegno personalizzato (NULL = sprite normale)
    unsigned groups;      // appartenenza veloce ai gruppi (Set di Phaser)
    int hasBody;
    Body body;
};

void sprite_init(Sprite *s, const Frame *f, float x, float y);
void sprite_setFrame(Sprite *s, const Frame *f);
void sprite_setScale(Sprite *s, float sc);
void sprite_setOrigin(Sprite *s, float ox, float oy);
void sprite_setVelocity(Sprite *s, float vx, float vy);
void sprite_setTint(Sprite *s, unsigned tint);
void sprite_setTintFill(Sprite *s, unsigned tint);
void sprite_clearTint(Sprite *s);
void anims_play(Sprite *s, const Animation *a, int startFrame);
void anims_pause(Sprite *s);
void anims_resume(Sprite *s);
void anims_stop(Sprite *s);
void sprite_animsUpdate(Sprite *s, float delta);  // UpdateList per uno sprite fuori dalla lista di disegno
int anims_generateFrameNames(const char *atlas, const char *prefix, int start, int end, int zeroPad, const Frame **out, int max);

// --- Lista di disegno (scene.children) e lista di aggiornamento --------------
void children_add(Sprite *s);
void children_remove(Sprite *s);
void children_reset(void);

// --- World (src/physics/arcade/World.js) --------------------------------------
typedef struct { Sprite **items; int n, cap; unsigned bit; } SpriteList;  // Group / Array; bit != 0: contains O(1)
void list_add(SpriteList *l, Sprite *s);
void list_remove(SpriteList *l, Sprite *s);
int list_index(const SpriteList *l, const Sprite *s);
int list_contains(const SpriteList *l, const Sprite *s);
void list_free(SpriteList *l);

// --- Tilemap (src/tilemaps: mappe Tiled ortogonali, un solo tileset) ---------
typedef struct {
    int index;  // gid, -1 = vuoto
    float rotation;
    unsigned char flipX, collides, faceTop, faceBottom, faceLeft, faceRight;
} Tile;
typedef struct {  // Tilemap + Tileset (addTilesetImage con margin/spacing)
    cJSON *json;
    int width, height, tileWidth, tileHeight;
    Texture2D tex;
    int firstgid, margin, spacing, columns;
    Tile *layerData[16];  // LayerData per nome (createLayer): condivisi dalle copie della stessa mappa
    const char *layerName[16];
    int n_layers;
} Tilemap;
typedef struct TilemapLayer {
    Sprite s;  // game object nella lista di disegno (depth, visible)
    Tilemap *map;
    Tile *data;
    float x, y;
    unsigned tint;  // forEachTile(tile.tint = c): la stessa tinta su tutte le tile del layer
    short *rowMin, *rowMax;  // tilemapLayer_draw: prima e ultima colonna non vuota per riga (rowMin > rowMax: riga vuota)
    unsigned rowsVer;        // versione delle tessere con cui sono state calcolate (tiles_ver in phaser.c)
} TilemapLayer;
Tilemap *make_tilemap(const char *jsonPath, const char *imagePath, int margin, int spacing);  // scene.make.tilemap + addTilesetImage
TilemapLayer *tilemap_createLayer(Tilemap *m, const char *name, double depth);  // NULL se il layer manca
void tilemap_resetLayers(Tilemap *m);  // a ogni partita: i layer si ricreano dal JSON
void tilemap_removeTilesWithin(Tilemap *m, const char *except, int x, int y, int w, int h);
cJSON *tilemap_getObjectLayerObjects(Tilemap *m, const char *name);
void tilemapLayer_setCollisionByProperty(TilemapLayer *l, const char *prop);  // { prop: true } + CalculateFacesWithin
Tile *tilemapLayer_getTileAtWorldXY(TilemapLayer *l, float x, float y);  // NULL fuori mappa o vuoto
void tilemapLayer_removeTileAt(TilemapLayer *l, int tx, int ty);  // RemoveTileAt(x, y, true, true): vuota e ricalcola le facce attorno

typedef void (*PairCallback)(void *ctx, Sprite *a, Sprite *b);
typedef enum { COLLIDE_ARRAYS, COLLIDE_GROUP_SELF, COLLIDE_TILEMAP } ColliderKind;
typedef struct {
    SpriteList *object1, *object2;
    TilemapLayer *layer;  // COLLIDE_TILEMAP: object1 contro il layer
    ColliderKind kind;
    PairCallback collideCallback;
    void *ctx;
    int overlapOnly, active;
} Collider;

Collider *physics_addCollider(SpriteList *a, SpriteList *b, PairCallback cb, void *ctx);
Collider *physics_addOverlap(SpriteList *a, SpriteList *b, PairCallback cb, void *ctx);
Collider *physics_addColliderSelf(SpriteList *g, PairCallback cb, void *ctx);
Collider *physics_addOverlapSelf(SpriteList *g, PairCallback cb, void *ctx);  // overlap(gruppo, gruppo): senza la coppia con se stesso
int physics_overlapCirc(float x, float y, float r);  // physics.overlapCirc(x, y, r, true).length
Collider *physics_addColliderLayer(SpriteList *g, TilemapLayer *l);  // physics.add.collider(sprite/gruppo, tilemapLayer)
Collider *physics_addColliderLayerCb(SpriteList *g, TilemapLayer *l, PairCallback cb, void *ctx);  // ... con collideCallback(sprite, tile = NULL)
void physics_existing(Sprite *s);  // scene.physics.add.existing
void physics_reset(void);
void world_update(float delta);
void world_postUpdate(void);

// --- Camera (startFollow, lerp 1) e disegno ----------------------------------
extern float world_gravityY;  // physics.world.gravity.y (le spell Jupiter e Mars)
extern float camera_scrollX, camera_scrollY;
extern float camera_followOffsetX;  // cameras.main.followOffset.x (scossone del boss del ponte)
extern int game_width, game_height;  // scale.setGameSize
void camera_follow(const Sprite *target);
void scene_update_list(float delta);  // UpdateList: preUpdate degli sprite (animazioni)
void scene_render(Shader fill);
extern int prof_children, prof_custom, prof_offscreen, prof_breaks, prof_tiles;
extern float render_zoom;  // 0 < render_zoom <= 1; set by the game before scene_render
void scene_render_one(Sprite *s, Shader fill);
extern int scene_fillActive;
void physics_drawDebug(void);  // World.drawDebug: corpi attivi  // un solo sprite con la pipeline normale


// --- Particelle (src/gameobjects/particles, sottoinsieme) --------------------
// Valori: {min,max} = decimale casuale all'emissione; {start,end} = lineare sulla vita.
typedef struct { float a, b; int kind; float (*ease)(float); } EmitterOp;  // kind: 0 fisso (a), 1 casuale min/max, 2 start/end (ease: NULL = Linear)
#define EOP(v) ((EmitterOp){ (v), (v), 0 })
#define ERND(lo, hi) ((EmitterOp){ (lo), (hi), 1 })
#define ERANGE(s, e) ((EmitterOp){ (s), (e), 2 })
typedef struct {
    const Frame *frames[24];
    int nframes, quantity, on, blendAdd, hasZone, nonRadial, flowEveryFrame;
    EmitterOp speed, speedY, angle, lifespan, alpha, scale, rotate, xOffset;  // nonRadial: velocita' = (speed, speedY)
    float gravityY, frequency, zoneRadius;  // zona: cerchio casuale centrato sul punto di emissione
    int collideBottom, collideTop;  // bounds con collideBottom / collideTop (Particle.checkBounds): bordi inferiore e superiore
    float bounce, boundsH;  // bounds.height; bounds.y sta nell'emettitore (cambia a runtime)
    int collideLeftRight;   // bounds con collideLeft/collideRight (di default veri): bordi x e x + boundsW
    float boundsX, boundsW;
    float rotateStep;       // rotate.onUpdate: angolo += rotateStep a ogni update
    int angleSteps;         // angle { start: angle.a, end: angle.b, steps }: EmitterOp.steppedEmit
    const Frame **frameRef; // texture condivisa che cambia (RenderTexture ridisegnata a ogni frame): fotogramma corrente
    int frameCycle;         // frame { frames, cycle: true }: in sequenza invece che a caso
    const Vector2 *edge;    // emitZone { type: 'edge', quantity }: punti in sequenza (yoyo falso)
    int nEdge;
    unsigned tint;
    unsigned tints[8];      // tint { onEmit: RND.pick(lista) }
    unsigned tintEnd;       // tint { start: tint, end: tintEnd }: interpolazione del numero intero (EmitterOp), non dei canali
    int hasTintEnd;
    int ntints;
    EmitterOp scaleY;       // scaleX / scaleY separati: scale fa da scaleX
    int hasScaleY;
    const Sprite *follow;
    int hasZoneRect;        // emitZone { type: 'random', source: Rectangle }: Rectangle.Random, relativo al punto di emissione
    Rectangle zoneRect;
} EmitterConfig;
typedef struct { float x, y, vx, vy, life, lifeCurrent, scale, scaleStart, alpha, alphaStart, angle, scaleY; unsigned tint; const Frame *frame; } Particle;
typedef struct ParticleEmitter {
    EmitterConfig c;
    Particle *alive;
    int n, cap;
    float counter, x, y, boundsY, stepCounter;
    int edgeIndex, frameIndex;
} ParticleEmitter;
typedef struct ParticleManager {  // ParticleEmitterManager: un game object della lista di disegno
    Sprite s;
    ParticleEmitter *emitters[8];
    int nemit, scrollFactor0;  // setScrollFactor(0): coordinate di schermo (scene UI)
    int hasWell;               // createGravityWell: un solo pozzo
    float wellX, wellY, wellPower, wellEpsilon;  // _power = power * gravity, _epsilon = epsilon^2
    int hasWell2;              // W_Spellstrom: un secondo pozzo, stessi power ed epsilon
    float well2X, well2Y;
} ParticleManager;
ParticleManager *particles_add(void);  // scene.add.particles
ParticleEmitter *particles_createEmitter(ParticleManager *m, EmitterConfig c);
void emitter_emitParticleAt(ParticleEmitter *e, float x, float y, int count);  // count <= 0: quantity
void manager_emitParticleAt(ParticleManager *m, float x, float y);
void particles_update_all(float delta);
int particles_alive(void);  // VS_PROF  // UpdateList: preUpdate dei manager
void particles_manager_update(ParticleManager *m, float delta);  // manager fuori dalla scena di gioco
void particles_manager_draw(ParticleManager *m);
void emitter_start(ParticleEmitter *e);
void emitter_stop(ParticleEmitter *e);

// --- rexShatterImage (plugin rex, GameObjects.ShatterImage): Mesh ortografica su una texture, spezzata in triangoli ---
typedef struct { float x, y, u, v, alpha; } MeshVertex;  // Geom.Mesh.Vertex (z = 0, colore bianco)
typedef struct { MeshVertex *vertex1, *vertex2, *vertex3; float _rotation, g; } MeshFace;  // ShatterImage Face
typedef struct ShatterImage {
    Sprite s;                 // x, y, scaleX/Y, alpha, depth; disegno: shatter_draw
    const Texture2D *tex;     // la texture della RenderTexture (isRenderTexture: v capovolta)
    float width, height;      // setSizeToFrame
    MeshVertex *vertices;
    MeshFace *faces;
    int nfaces;
    float variation;
    int samplesPerRing;
    TweenRef task;
    int hasTask;
} ShatterImage;
ShatterImage *shatterImage_add(float x, float y, const Texture2D *tex, float w, float h);  // scene.add.rexShatterImage
void shatterImage_shatter(ShatterImage *im);  // shatter() al centro
TweenProp face_prop(MeshFace *f, int which, float end);  // which: 0 alpha, 1 angle, 2 x, 3 y (proprieta' di Face per i tween)

// --- Varie: physics.closest, velocityFromRotation, Graphics circle ------------
Sprite *physics_closest(const Sprite *from, SpriteList *targets);
Vector2 velocityFromRotation(float rad, float speed);

#endif
