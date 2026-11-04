// Traduzione in C del sottoinsieme di Phaser 3.55.2 usato dal gioco.
// Ogni funzione riporta il file e il metodo originale da cui viene.

#include "phaser.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rlgl.h"
extern int prof_tiles;
static void manager_draw(Sprite *s);

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float dist(float x1, float y1, float x2, float y2) { float dx = x1 - x2, dy = y1 - y2; return sqrtf(dx * dx + dy * dy); }

// ============================================================================
// gmem: allocazioni di una partita, liberate in blocco da gmem_reset (intestazione = indice nella tabella)
// ============================================================================
typedef union { size_t idx; long double ld; long long ll; void *p; } GHead;  // allineamento massimo senza max_align_t (assente nella toolchain Vita)
static void **gtab;
static size_t gn, gcap;
static void gtrack(GHead *h) {
    if (gn == gcap) { gcap = gcap ? gcap * 2 : 1024; gtab = realloc(gtab, gcap * sizeof(void *)); }
    h->idx = gn;
    gtab[gn++] = h;
}
void *gmem_calloc(size_t n, size_t size) {
    GHead *h = calloc(1, sizeof(GHead) + n * size);
    if (!h) return NULL;
    gtrack(h);
    return h + 1;
}
void *gmem_realloc(void *p, size_t size) {
    if (!p) { GHead *h = malloc(sizeof(GHead) + size); if (!h) return NULL; gtrack(h); return h + 1; }
    GHead *h = (GHead *)p - 1;
    size_t i = h->idx;
    h = realloc(h, sizeof(GHead) + size);
    if (!h) return NULL;
    gtab[i] = h;
    return h + 1;
}
void gmem_free(void *p) {
    if (!p) return;
    GHead *h = (GHead *)p - 1;
    size_t i = h->idx;
    GHead *last = gtab[--gn];
    gtab[i] = last;
    last->idx = i;
    free(h);
}
void gmem_reset(void) {
    for (size_t i = 0; i < gn; i++) free(gtab[i]);
    gn = 0;
}
size_t gmem_count(void) { return gn; }

// ============================================================================
// src/time/Clock.js + TimerEvent.js
// ============================================================================
#define MAX_TIMERS 4096
static TimerEvent timers[MAX_TIMERS];
static int active_[MAX_TIMERS], n_active, pending_ins[MAX_TIMERS], n_ins, pending_rem[MAX_TIMERS], n_rem;

void time_reset(void) {
    for (int i = 0; i < MAX_TIMERS; i++) { unsigned g = timers[i].gen; timers[i] = (TimerEvent){ 0 }; timers[i].gen = g + 1; }
    n_active = n_ins = n_rem = 0;
}

// Clock.addEvent -> TimerEvent.reset
Timer time_addEvent(float delay, int loop, int repeat, Callback cb, void *ctx) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        TimerEvent *e = &timers[i];
        if (e->active) continue;
        unsigned g = e->gen + 1;
        *e = (TimerEvent){ .delay = delay, .timeScale = 1, .callback = cb, .ctx = ctx, .active = 1, .gen = g };
        e->repeatCount = (repeat == -1 || loop) ? 999999999999.0 : repeat;
        pending_ins[n_ins++] = i;
        return (Timer){ i, g };
    }
    return (Timer){ -1, 0 };
}

TimerEvent *time_get(Timer t) {
    if (t.idx < 0 || t.idx >= MAX_TIMERS) return NULL;
    TimerEvent *e = &timers[t.idx];
    return (e->active && e->gen == t.gen) ? e : NULL;
}

// Clock.removeEvent: in coda per la rimozione al prossimo preUpdate
void time_removeEvent(Timer t) {
    TimerEvent *e = time_get(t);
    if (!e || e->pendingRemoval) return;
    e->pendingRemoval = 1;
    pending_rem[n_rem++] = t.idx;
}

// Clock.preUpdate
void clock_preUpdate(void) {
    for (int i = 0; i < n_rem; i++) {
        int idx = pending_rem[i];
        for (int k = 0; k < n_active; k++)
            if (active_[k] == idx) { memmove(&active_[k], &active_[k + 1], (n_active - k - 1) * sizeof(int)); n_active--; break; }
        for (int k = 0; k < n_ins; k++)  // rimosso prima di entrare: mai attivato
            if (pending_ins[k] == idx) { memmove(&pending_ins[k], &pending_ins[k + 1], (n_ins - k - 1) * sizeof(int)); n_ins--; break; }
        timers[idx].active = 0;  // event.destroy()
        timers[idx].gen++;
    }
    for (int i = 0; i < n_ins; i++) active_[n_active++] = pending_ins[i];
    n_rem = n_ins = 0;
}

// Clock.update
void clock_update(float delta) {
    for (int i = 0; i < n_active; i++) {
        TimerEvent *e = &timers[active_[i]];
        if (e->paused || e->pendingRemoval) continue;
        e->elapsed += delta * e->timeScale;
        if (e->elapsed >= e->delay) {
            float remainder = e->elapsed - e->delay;
            e->elapsed = e->delay;
            unsigned g = e->gen;
            if (!e->hasDispatched && e->callback) {
                e->hasDispatched = 1;
                e->callback(e->ctx);
            }
            if (e->gen != g || !e->active) continue;  // distrutto nel callback
            if (e->repeatCount > 0) {
                e->repeatCount--;
                e->elapsed = remainder;
                e->hasDispatched = 0;
            } else if (!e->pendingRemoval) {
                e->pendingRemoval = 1;
                pending_rem[n_rem++] = active_[i];
            }
        }
    }
}

// ============================================================================
// src/tweens/TweenManager.js + tween/Tween.js (ease Linear)
// ============================================================================
#define MAX_TWEENS 4096
static Tween tweens[MAX_TWEENS];
static int t_add[MAX_TWEENS], n_tadd, t_active[MAX_TWEENS], n_tactive, t_destroy[MAX_TWEENS], n_tdestroy, t_pending[MAX_TWEENS], n_tpending;

void tweens_reset(void) {
    for (int i = 0; i < MAX_TWEENS; i++) {
        unsigned g = tweens[i].gen;
        if (tweens[i].data && tweens[i].data != tweens[i].inl) free(tweens[i].data);
        tweens[i] = (Tween){ 0 };
        tweens[i].gen = g + 1;
    }
    n_tadd = n_tactive = n_tdestroy = n_tpending = 0;
}

int tweens_used(void) { int n = 0; for (int i = 0; i < MAX_TWEENS; i++) n += tweens[i].used; return n; }  // VS_PROF
Tween *tween_get(TweenRef r) {
    if (r.idx < 0 || r.idx >= MAX_TWEENS) return NULL;
    Tween *t = &tweens[r.idx];
    return (t->used && t->gen == r.gen) ? t : NULL;
}

static float td_get(TweenData *d) { return d->get ? d->get(d->o) : d->t2 ? (*d->t + *d->t2) * 0.5f : *d->t; }  // getter 'scale'
static void td_set(TweenData *d, float v) {
    if (d->set) { d->set(d->o, v); return; }
    *d->t = v;
    if (d->t2) *d->t2 = v;
}

static void list_del(int *arr, int *n, int idx) {
    for (int k = 0; k < *n; k++)
        if (arr[k] == idx) { memmove(&arr[k], &arr[k + 1], (*n - k - 1) * sizeof(int)); (*n)--; return; }
}

static void makeActive(int idx) {  // TweenManager.makeActive
    Tween *t = &tweens[idx];
    if (t->inAdd || t->inActive) return;
    if (t->inPending) { list_del(t_pending, &n_tpending, idx); t->inPending = 0; }
    t->inAdd = 1;
    t_add[n_tadd++] = idx;
    t->state = TS_PENDING_ADD;
}

// src/math/easing/bounce/Out.js, sine/InOut.js
float Ease_BounceOut(float v) {
    if (v < 1 / 2.75f) return 7.5625f * v * v;
    if (v < 2 / 2.75f) { v -= 1.5f / 2.75f; return 7.5625f * v * v + 0.75f; }
    if (v < 2.5f / 2.75f) { v -= 2.25f / 2.75f; return 7.5625f * v * v + 0.9375f; }
    v -= 2.625f / 2.75f;
    return 7.5625f * v * v + 0.984375f;
}
float Ease_SineInOut(float v) { return 0.5f * (1 - cosf(PI * v)); }
float Ease_SineOut(float v) { return sinf(v * PI / 2); }  // src/math/easing/sine/Out.js
float Ease_BackOut(float v) { v -= 1; return v * v * ((1.70158f + 1) * v + 1.70158f) + 1; }  // src/math/easing/back/Out.js (overshoot 1.70158)
float Ease_ExpoOut(float v) { return v == 1 ? 1 : 1 - powf(2, -10 * v); }  // src/math/easing/expo/Out.js
float Ease_ExpoIn(float v) { return powf(2, 10 * (v - 1)) - 0.001f; }  // src/math/easing/expo/In.js
float Ease_QuintOut(float v) { v -= 1; return v * v * v * v * v + 1; }  // src/math/easing/quintic/Out.js

// TweenManager.add -> createTweenData
TweenRef tweens_add(TweenConfig c) {
    for (int i = 0; i < MAX_TWEENS; i++) {
        Tween *t = &tweens[i];
        if (t->used) continue;
        unsigned g = t->gen + 1;
        if (t->data && t->data != t->inl) free(t->data);
        *t = (Tween){ .used = 1, .gen = g, .timeScale = 1, .onComplete = c.onComplete, .onRepeat = c.onRepeat, .onYoyo = c.onYoyo, .onStart = c.onStart, .onUpdate = c.onUpdate, .ctx = c.ctx, .paused = c.paused, .persist = c.persist };
        const TweenProp *props = c.many ? c.many : c.props;
        t->totalData = c.many ? c.nmany : c.nprops;
        t->data = t->totalData > TWEEN_DATA ? malloc(t->totalData * sizeof(TweenData)) : t->inl;
        t->loop = c.loop == -1 ? 999999999999.0 : c.loop;
        t->loopCounter = t->loop;
        for (int k = 0; k < t->totalData; k++) {
            TweenData *d = &t->data[k];
            *d = (TweenData){ .t = props[k].t, .t2 = props[k].t2, .endCfg = props[k].end, .yoyo = c.yoyo, .ease = c.ease,
                              .get = props[k].get, .set = props[k].set, .o = props[k].o };
            d->delay = c.delay;
            d->duration = fmaxf(c.duration, 0.001f);
            d->repeat = c.repeat;
            d->state = TD_CREATED;
        }
        makeActive(i);
        return (TweenRef){ i, g };
    }
    return (TweenRef){ -1, 0 };
}

// Tween.resetTweenData
static void resetTweenData(Tween *t, int resetFromLoop) {
    for (int i = 0; i < t->totalData; i++) {
        TweenData *d = &t->data[i];
        d->elapsed = 0;
        d->repeatCounter = d->repeat == -1 ? 999999999999.0 : d->repeat;
        if (resetFromLoop) {
            d->start = td_get(d);  // getStartValue: valore attuale del target
            d->end = d->endCfg;
            d->current = d->start;
            d->state = TD_PLAYING_FORWARD;
        } else {
            d->state = TD_PENDING_RENDER;
        }
        if (d->delay > 0) {
            d->elapsed = d->delay;
            d->state = TD_DELAY;
        }
    }
}

// Tween.setStateFromEnd / setStateFromStart (senza hold, repeatDelay, flip)
static int setStateFromEnd(Tween *t, TweenData *d, float diff) {
    // getStartValue(target, key, tweenData.start): per un valore numerico getStart restituisce l'argomento,
    // quindi start resta quello di partenza (lo yoyo torna davvero indietro, il repeat riparte da capo)
    if (d->yoyo) {
        d->elapsed = diff;
        if (t->onYoyo) t->onYoyo(t->ctx);  // dispatchTweenDataEvent(TWEEN_YOYO)
        return TD_PLAYING_BACKWARD;
    }
    if (d->repeatCounter > 0) {
        d->repeatCounter--;
        d->elapsed = diff;
        d->end = d->endCfg;
        if (t->onRepeat) t->onRepeat(t->ctx);
        return TD_PLAYING_FORWARD;
    }
    return TD_COMPLETE;
}
static int setStateFromStart(Tween *t, TweenData *d, float diff) {
    if (d->repeatCounter > 0) {
        d->repeatCounter--;
        d->elapsed = diff;
        d->end = d->endCfg;
        if (t->onRepeat) t->onRepeat(t->ctx);
        return TD_PLAYING_FORWARD;
    }
    return TD_COMPLETE;
}

// Tween.updateTweenData
static int updateTweenData(Tween *t, TweenData *d, float delta) {
    switch (d->state) {
    case TD_PLAYING_FORWARD:
    case TD_PLAYING_BACKWARD: {
        float elapsed = d->elapsed + delta, duration = d->duration, diff = 0;
        if (elapsed > duration) { diff = elapsed - duration; elapsed = duration; }
        int forward = d->state == TD_PLAYING_FORWARD;
        float progress = elapsed / duration;
        d->elapsed = elapsed;
        if (progress == 1) {
            if (forward) {
                d->current = d->end;
                td_set(d, d->end);
                d->state = setStateFromEnd(t, d, diff);
            } else {
                d->current = d->start;
                td_set(d, d->start);
                d->state = setStateFromStart(t, d, diff);
            }
        } else {
            float v = forward ? progress : 1 - progress;
            if (d->ease) v = d->ease(v);  // tweenData.ease(progress)
            d->current = d->start + (d->end - d->start) * v;
            td_set(d, d->current);
        }
        break;
    }
    case TD_DELAY:
        d->elapsed -= delta;
        if (d->elapsed <= 0) {
            d->elapsed = fabsf(d->elapsed);
            d->state = TD_PENDING_RENDER;
        }
        break;
    case TD_PENDING_RENDER:
        if (!t->hasStarted) { t->hasStarted = 1; if (t->onStart) t->onStart(t->ctx); }
        d->start = td_get(d);
        d->end = d->endCfg;
        d->current = d->start;
        td_set(d, d->start);
        d->state = TD_PLAYING_FORWARD;
        break;
    }
    return d->state != TD_COMPLETE;
}

// Tween.nextState (senza loopDelay, completeDelay)
static void nextState(Tween *t) {
    if (t->loopCounter > 0) {
        t->loopCounter--;
        resetTweenData(t, 1);
        t->state = TS_ACTIVE;
    } else {
        t->state = TS_PENDING_REMOVE;
        if (t->onComplete) t->onComplete(t->ctx);
    }
}

// Tween.update
static int tween_update(Tween *t, float delta) {
    delta *= t->timeScale;
    if (t->state == TS_ACTIVE) {
        int stillRunning = 0;
        for (int i = 0; i < t->totalData; i++)
            if (updateTweenData(t, &t->data[i], delta)) stillRunning = 1;
        if (t->onUpdate && t->hasStarted) t->onUpdate(t->ctx);
        if (!stillRunning) nextState(t);
    }
    return t->state == TS_PENDING_REMOVE;
}

// Tween.init (false se in pausa: resta in attesa)
static int tween_init(Tween *t) {
    if (t->paused) {
        t->state = TS_PENDING_ADD;
        t->pausedState = TS_INIT;
        return 0;
    }
    t->state = TS_INIT;
    return 1;
}

// Tween.seek(0): riporta all'inizio e avanza di 16.6 ms come l'originale
static void tween_seek0(int idx) {
    Tween *t = &tweens[idx];
    t->hasStarted = 0;
    if (t->state == TS_REMOVED) makeActive(idx);
    for (int i = 0; i < t->totalData; i++) {
        TweenData *d = &t->data[i];
        d->elapsed = 0;
        d->repeatCounter = d->repeat == -1 ? 999999999999.0 : d->repeat;
        d->current = d->start;
        d->state = TD_PLAYING_FORWARD;
        updateTweenData(t, d, 0);
        if (d->delay > 0) {
            d->elapsed = d->delay;
            d->state = TD_DELAY;
        }
    }
    int wasPaused = 0;
    if (t->state == TS_PAUSED) { wasPaused = 1; t->state = TS_ACTIVE; }
    // do { update(0, 16.6) } while (totalProgress < 0): una volta sola. update() avanza solo da ACTIVE.
    tween_update(t, 16.6f);
    if (wasPaused) t->state = TS_PAUSED;
}

// Tween.play
void tween_play(TweenRef r) {
    Tween *t = tween_get(r);
    if (!t) return;
    int state = t->state;
    if (state == TS_INIT) { resetTweenData(t, 0); t->state = TS_ACTIVE; return; }
    if (state == TS_ACTIVE || (state == TS_PENDING_ADD && t->pausedState == TS_PENDING_ADD)) return;
    if (state == TS_PENDING_REMOVE || state == TS_REMOVED) { tween_seek0(r.idx); makeActive(r.idx); return; }
    if (t->paused) { t->paused = 0; makeActive(r.idx); return; }  // se e' gia' attivo, makeActive non fa nulla
    resetTweenData(t, 0);
    t->state = TS_ACTIVE;
    makeActive(r.idx);
}

// Tween.resume
void tween_resume(TweenRef r) {
    Tween *t = tween_get(r);
    if (!t) return;
    if (t->state == TS_PAUSED) { t->paused = 0; t->state = t->pausedState; }
    else tween_play(r);
}

// Tween.restart
void tween_restart(TweenRef r) {
    Tween *t = tween_get(r);
    if (!t) return;
    if (t->state == TS_ACTIVE) tween_seek0(r.idx);
    else if (t->state == TS_REMOVED) { tween_seek0(r.idx); makeActive(r.idx); }
    else if (t->state == TS_PENDING_ADD) return;
    else tween_play(r);
}

// Tween.stop
void tween_stop(TweenRef r) {
    Tween *t = tween_get(r);
    if (!t || t->state == TS_REMOVED) return;
    if (t->state == TS_PAUSED || t->state == TS_PENDING_ADD) t_destroy[n_tdestroy++] = r.idx;
    t->state = TS_PENDING_REMOVE;
}

// TweenManager.remove: fuori da tutte le liste
void tween_remove(TweenRef r) {
    Tween *t = tween_get(r);
    if (!t) return;
    if (t->inActive) list_del(t_active, &n_tactive, r.idx);
    if (t->inAdd) list_del(t_add, &n_tadd, r.idx);
    if (t->inPending) list_del(t_pending, &n_tpending, r.idx);
    list_del(t_destroy, &n_tdestroy, r.idx);
    t->inActive = t->inAdd = t->inPending = 0;
    t->state = TS_REMOVED;
    t->used = 0;  // semplificazione: il gioco non riusa mai un tween dopo remove, lo slot si libera
}

// Tween.pause / isPaused
void tween_pause(TweenRef r) {
    Tween *t = tween_get(r);
    if (!t || t->state == TS_PAUSED) return;
    t->paused = 1;
    t->pausedState = t->state;
    t->state = TS_PAUSED;
}
int tween_isPaused(TweenRef r) { Tween *t = tween_get(r); return t && t->state == TS_PAUSED; }

// TweenManager.preUpdate
void tweens_preUpdate(void) {
    for (int i = 0; i < n_tdestroy; i++) {
        int idx = t_destroy[i];
        Tween *t = &tweens[idx];
        if (t->inActive) { list_del(t_active, &n_tactive, idx); t->inActive = 0; t->state = TS_REMOVED; }
        else if (t->inPending) { list_del(t_pending, &n_tpending, idx); t->inPending = 0; t->state = TS_REMOVED; }
        // In JS il tween rimosso vive finche' qualcuno lo referenzia; qui lo slot torna libero (i riferimenti vecchi
        // diventano nulli grazie a gen), tranne per i tween che il gioco fa ripartire dopo la fine (persist)
        if (t->state == TS_REMOVED && !t->persist && !t->inAdd) t->used = 0;
    }
    n_tdestroy = 0;
    for (int i = 0; i < n_tadd; i++) {
        int idx = t_add[i];
        Tween *t = &tweens[idx];
        t->inAdd = 0;
        if (t->state == TS_PENDING_ADD) {
            if (tween_init(t)) {
                tween_play((TweenRef){ idx, t->gen });
                t->inActive = 1;
                t_active[n_tactive++] = idx;
            } else {
                t->inPending = 1;
                t_pending[n_tpending++] = idx;
            }
        }
    }
    n_tadd = 0;
}

// TweenManager.update
void tweens_update(float delta) {
    for (int i = 0; i < n_tactive; i++) {
        int idx = t_active[i];
        if (tween_update(&tweens[idx], delta)) t_destroy[n_tdestroy++] = idx;
    }
}

// ============================================================================
// src/animations/AnimationState.js + Animation.js
// ============================================================================
int anims_generateFrameNames(const char *atlas, const char *prefix, int start, int end, int zeroPad, const Frame **out, int max) {
    char name[128];
    int n = 0;
    for (int i = start; i <= end && n < max; i++) {
        snprintf(name, sizeof(name), "%s%0*d.png", prefix, zeroPad, i);
        const Frame *f = frame_get(atlas, name);
        if (f) out[n++] = f;  // generateFrameNames salta i fotogrammi che non esistono
    }
    return n;
}

void anims_stop(Sprite *s) { s->anims.isPlaying = 0; }

// AnimationState.play -> startAnimation -> load + getFirstTick + handleStart
void anims_play(Sprite *s, const Animation *a, int startFrame) {
    AnimationState *st = &s->anims;
    if (st->isPlaying) anims_stop(s);
    if (!a || !a->n) return;
    st->cur = a;
    if (startFrame > a->n) startFrame = 0;
    st->index = startFrame < a->n ? startFrame : 0;  // semplificazione: frames[n] e' undefined in JS; qui si riparte da 0
    st->repeatCounter = a->repeat == -1 ? 0x7fffffff : a->repeat;
    st->accumulator = 0;
    st->nextTick = a->msPerFrame;
    st->isPlaying = 1;
    st->paused = 0;
    if (st->timeScale == 0) st->timeScale = 1;
    sprite_setFrame(s, a->frames[st->index]);  // handleStart -> setCurrentFrame
}

// AnimationState.pause / resume
void anims_pause(Sprite *s) { AnimationState *st = &s->anims; if (!st->paused) { st->paused = 1; st->wasPlaying = st->isPlaying; st->isPlaying = 0; } }
void anims_resume(Sprite *s) { AnimationState *st = &s->anims; if (st->paused) { st->paused = 0; st->isPlaying = st->wasPlaying; } }

static void anim_nextFrame(Sprite *s) {  // Animation.nextFrame
    AnimationState *st = &s->anims;
    const Animation *a = st->cur;
    if (st->index == a->n - 1) {
        if (st->repeatCounter > 0) {  // repeatAnimation
            st->repeatCounter--;
            st->index = 0;
            sprite_setFrame(s, a->frames[0]);
            st->accumulator -= st->nextTick;  // getNextTick
            st->nextTick = a->msPerFrame;
        } else {  // complete
            st->isPlaying = 0;
            if (s->onAnimationComplete) s->onAnimationComplete(s->owner);
        }
    } else {
        st->index++;
        sprite_setFrame(s, a->frames[st->index]);
        st->accumulator -= st->nextTick;
        st->nextTick = a->msPerFrame;
    }
}

// AnimationState.update (skipMissedFrames = true)
static void anims_update(Sprite *s, float delta) {
    AnimationState *st = &s->anims;
    if (!st->isPlaying || !st->cur) return;
    st->accumulator += delta * st->timeScale;
    if (st->accumulator >= st->nextTick) {
        anim_nextFrame(s);
        int safetyNet = 0;
        while (st->isPlaying && st->accumulator > st->nextTick && safetyNet < 60) {
            anim_nextFrame(s);
            safetyNet++;
        }
    }
}

// ============================================================================
// Sprite (GameObjects.Sprite + componenti Transform/Origin/Flip/Tint/Alpha)
// ============================================================================
void sprite_setFrame(Sprite *s, const Frame *f) {  // Sprite.setFrame -> setSizeToFrame
    if (!f) return;
    s->frame = f;
    s->width = f->sw;
    s->height = f->sh;
}

void sprite_init(Sprite *s, const Frame *f, float x, float y) {
    *s = (Sprite){ .x = x, .y = y, .scaleX = 1, .scaleY = 1, .originX = 0.5f, .originY = 0.5f, .alpha = 1,
                   .visible = 1, .active = 1, .tint = 0xffffff };
    s->anims.timeScale = 1;
    sprite_setFrame(s, f);
}

void sprite_setScale(Sprite *s, float sc) { s->scaleX = s->scaleY = sc; }
void sprite_setOrigin(Sprite *s, float ox, float oy) { s->originX = ox; s->originY = oy; }
void sprite_setTint(Sprite *s, unsigned t) { s->tint = t; s->tintFill = 0; }
void sprite_setTintFill(Sprite *s, unsigned t) { s->tint = t; s->tintFill = 1; }
void sprite_clearTint(Sprite *s) { sprite_setTint(s, 0xffffff); }
void sprite_setVelocity(Sprite *s, float vx, float vy) { s->body.velocity = (Vector2){ vx, vy }; }

// ============================================================================
// src/physics/arcade/Body.js
// ============================================================================
void body_updateCenter(Body *b) {
    b->center = (Vector2){ b->position.x + b->halfWidth, b->position.y + b->halfHeight };
}

// Body constructor
static void body_create(Sprite *s) {
    Body *b = &s->body;
    *b = (Body){ 0 };
    b->gameObject = s;
    float width = s->width * s->scaleX, height = s->height * s->scaleY;  // displayWidth
    if (!width) width = 64;
    if (!height) height = 64;
    b->enable = 1;
    b->position = (Vector2){ s->x - s->scaleX * s->originX * s->width, s->y - s->scaleY * s->originY * s->height };
    b->prev = b->prevFrame = b->position;
    b->allowRotation = 1;
    b->rotation = b->preRotation = s->angle;
    b->width = width;
    b->height = height;
    b->sourceWidth = s->width;  // frame.realWidth
    b->sourceHeight = s->height;
    b->halfWidth = fabsf(width / 2);
    b->halfHeight = fabsf(height / 2);
    b->center = (Vector2){ b->position.x + b->halfWidth, b->position.y + b->halfHeight };
    b->maxVelocity = (Vector2){ 10000, 10000 };
    b->mass = 1;
    b->pushable = 1;
    b->moves = 1;
    b->checkCollision = (Flags){ 0, 1, 1, 1, 1 };
    b->touching = b->wasTouching = b->blocked = (Flags){ 1, 0, 0, 0, 0 };
    b->_sx = s->scaleX;
    b->_sy = s->scaleY;
    s->hasBody = 1;
}

void body_setOffset(Body *b, float x, float y) { b->offset = (Vector2){ x, y }; }

void body_updateCenter(Body *b);
void body_setSize(Body *b, float width, float height) {  // Body.setSize(width, height, center = true)
    Sprite *s = b->gameObject;
    b->sourceWidth = width, b->sourceHeight = height;
    b->width = b->sourceWidth * b->_sx, b->height = b->sourceHeight * b->_sy;
    b->halfWidth = floorf(b->width / 2), b->halfHeight = floorf(b->height / 2);
    if (s) b->offset = (Vector2){ (s->width - width) / 2, (s->height - height) / 2 };
    b->isCircle = 0, b->radius = 0;
    body_updateCenter(b);
}

void body_setCircle(Body *b, float radius, float offsetX, float offsetY) {
    if (radius > 0) {
        b->isCircle = 1;
        b->radius = radius;
        b->sourceWidth = radius * 2;
        b->sourceHeight = radius * 2;
        b->width = b->sourceWidth * b->_sx;
        b->height = b->sourceHeight * b->_sy;
        b->halfWidth = floorf(b->width / 2);
        b->halfHeight = floorf(b->height / 2);
        b->offset = (Vector2){ offsetX, offsetY };
        body_updateCenter(b);
    } else {
        b->isCircle = 0;
    }
}

static void body_updateBounds(Body *b) {
    Sprite *s = b->gameObject;
    b->transform.x = s->x;
    b->transform.y = s->y;
    b->transform.rotation = s->angle;
    b->transform.scaleX = s->scaleX;
    b->transform.scaleY = s->scaleY;
    b->transform.displayOriginX = s->originX * s->width;
    b->transform.displayOriginY = s->originY * s->height;
    float asx = fabsf(s->scaleX), asy = fabsf(s->scaleY);
    if (b->_sx != asx || b->_sy != asy) {
        b->width = b->sourceWidth * asx;
        b->height = b->sourceHeight * asy;
        b->_sx = asx;
        b->_sy = asy;
        b->halfWidth = floorf(b->width / 2);
        b->halfHeight = floorf(b->height / 2);
        body_updateCenter(b);
    }
}

static void body_updateFromGameObject(Body *b) {
    body_updateBounds(b);
    b->position.x = b->transform.x + b->transform.scaleX * (b->offset.x - b->transform.displayOriginX);
    b->position.y = b->transform.y + b->transform.scaleY * (b->offset.y - b->transform.displayOriginY);
    body_updateCenter(b);
}

static void body_resetFlags(Body *b) {
    b->wasTouching = b->touching;
    b->touching = (Flags){ 1, 0, 0, 0, 0 };
    b->blocked = (Flags){ 1, 0, 0, 0, 0 };
    b->overlapR = b->overlapX = b->overlapY = 0;
    b->embedded = 0;
}

// World.computeVelocity: gravita' del mondo e limiti (accelerazione e drag: mai usati)
float world_gravityY;
static void world_updateMotion(Body *b, float delta) {
    if (!b->noGravity) b->velocity.y += world_gravityY * delta;  // allowGravity: gravita' del mondo (quella del corpo e' sempre 0)
    b->velocity.x = clampf(b->velocity.x, -b->maxVelocity.x, b->maxVelocity.x);
    b->velocity.y = clampf(b->velocity.y, -b->maxVelocity.y, b->maxVelocity.y);
}

// Body.checkWorldBounds (world.checkCollision: tutti i lati)
static int body_checkWorldBounds(Body *b) {
    const Rectangle *r = b->customBounds;
    float bx = b->hasWorldBounce ? -b->worldBounce.x : -b->bounce.x, by = b->hasWorldBounce ? -b->worldBounce.y : -b->bounce.y;
    int set = 0;
    if (b->position.x < r->x) b->position.x = r->x, b->velocity.x *= bx, b->blocked.left = 1, set = 1;
    else if (body_right(b) > r->x + r->width) b->position.x = r->x + r->width - b->width, b->velocity.x *= bx, b->blocked.right = 1, set = 1;
    if (b->position.y < r->y) b->position.y = r->y, b->velocity.y *= by, b->blocked.up = 1, set = 1;
    else if (body_bottom(b) > r->y + r->height) b->position.y = r->y + r->height - b->height, b->velocity.y *= by, b->blocked.down = 1, set = 1;
    if (set) { b->blocked.none = 0; body_updateCenter(b); }
    return set;
}
static void body_update(Body *b, float delta) {
    b->prev = b->position;
    if (b->moves) {
        world_updateMotion(b, delta);
        float vx = b->velocity.x, vy = b->velocity.y;
        b->newVelocity = (Vector2){ vx * delta, vy * delta };
        b->position.x += b->newVelocity.x;
        b->position.y += b->newVelocity.y;
        body_updateCenter(b);
        // b->angle = atan2(vy, vx) e b->speed = |v| di Phaser: nessuno li legge, e sulla Vita atan2f per ogni corpo a ogni
        // passo (nemici + gemme, ~700) costava millisecondi

        if (b->collideWorldBounds && b->customBounds && body_checkWorldBounds(b) && b->onWorldBounds) b->onWorldBounds(b->onWorldBoundsCtx);
    }
    b->_dx = b->position.x - b->prev.x;
    b->_dy = b->position.y - b->prev.y;
}

static void body_preUpdate(Body *b, int willStep, float delta) {
    if (willStep) body_resetFlags(b);
    body_updateFromGameObject(b);
    b->rotation = b->transform.rotation;
    b->preRotation = b->rotation;
    if (b->moves) {
        b->prev = b->position;
        b->prevFrame = b->position;
    }
    if (willStep) body_update(b, delta);
}

static void body_postUpdate(Body *b) {
    float dx = b->position.x - b->prevFrame.x, dy = b->position.y - b->prevFrame.y;
    if (b->moves) {
        float mx = b->deltaMax.x, my = b->deltaMax.y;
        if (mx != 0 && dx != 0) { if (dx < 0 && dx < -mx) dx = -mx; else if (dx > 0 && dx > mx) dx = mx; }
        if (my != 0 && dy != 0) { if (dy < 0 && dy < -my) dy = -my; else if (dy > 0 && dy > my) dy = my; }
        b->gameObject->x += dx;
        b->gameObject->y += dy;
    }
    // allowRotation: gameObject.angle += deltaZ() = rotation - preRotation = 0 (nessuna velocita' angolare)
    b->_tx = dx;
    b->_ty = dy;
}

static float deltaAbsX(const Body *b) { return b->_dx > 0 ? b->_dx : -b->_dx; }
static float deltaAbsY(const Body *b) { return b->_dy > 0 ? b->_dy : -b->_dy; }

// ============================================================================
// src/tilemaps: ParseTileLayers (Tiled JSON), Tileset, TilemapLayer, CalculateFacesWithin
// ============================================================================
static void ParseGID(unsigned gid, Tile *t) {  // tilemaps/parsers/tiled/ParseGID.js
    int h = (gid & 0x80000000u) != 0, v = (gid & 0x40000000u) != 0, d = (gid & 0x20000000u) != 0;
    float r = 0;
    int f = 0;
    if (h && v && d) r = PI / 2, f = 1;
    else if (h && v) r = PI;
    else if (h && d) r = PI / 2;
    else if (h) f = 1;
    else if (v && d) r = 3 * PI / 2;
    else if (v) r = PI, f = 1;
    else if (d) r = 3 * PI / 2, f = 1;
    gid &= ~(0x80000000u | 0x40000000u | 0x20000000u);
    t->index = gid ? (int)gid : -1;  // gid 0 = nessun tile (index -1)
    t->rotation = r;
    t->flipX = (unsigned char)f;
}

Tilemap *make_tilemap(const char *jsonPath, const char *imagePath, int margin, int spacing) {
    cJSON *j = read_json(jsonPath);
    if (!j) return NULL;
    Tilemap *m = calloc(1, sizeof(Tilemap));
    m->json = j;
    m->width = cJSON_GetObjectItem(j, "width")->valueint;
    m->height = cJSON_GetObjectItem(j, "height")->valueint;
    m->tileWidth = cJSON_GetObjectItem(j, "tilewidth")->valueint;
    m->tileHeight = cJSON_GetObjectItem(j, "tileheight")->valueint;
    cJSON *ts = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "tilesets"), 0);
    m->firstgid = ts ? cJSON_GetObjectItem(ts, "firstgid")->valueint : 1;
    m->margin = margin, m->spacing = spacing;
    static char texKey[16][96];  // textures.get(key): una texture per immagine anche se piu' mappe la usano (Inverse)
    static Texture2D texCache[16];
    int k = 0;
    while (k < 16 && texKey[k][0] && strcmp(texKey[k], imagePath)) k++;
    if (k < 16 && texKey[k][0]) m->tex = texCache[k];
    else {
        m->tex = LoadTexture(vs_path(imagePath));
        SetTextureFilter(m->tex, TEXTURE_FILTER_POINT);
        if (k < 16) snprintf(texKey[k], sizeof(texKey[k]), "%s", imagePath), texCache[k] = m->tex;
    }
    // Tileset.updateTileData: colonne = (larghezza - 2 margin + spacing) / (tile + spacing)
    m->columns = (m->tex.width - margin * 2 + spacing) / (m->tileWidth + spacing);
    return m;
}

void tilemap_resetLayers(Tilemap *m) {  // make.tilemap: LayerData nuove dalla cache JSON (removeTile non sopravvive alla partita)
    for (int i = 0; i < m->n_layers; i++) free(m->layerData[i]);
    m->n_layers = 0;
}

static cJSON *tilemap_layerJSON(Tilemap *m, const char *name) {
    cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItem(m->json, "layers"))
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(l, "name")), name)) return l;
    return NULL;
}

cJSON *tilemap_getObjectLayerObjects(Tilemap *m, const char *name) {
    return cJSON_GetObjectItem(tilemap_layerJSON(m, name), "objects");
}

static void tilemapLayer_draw(Sprite *s);
static unsigned tiles_ver = 1;  // cresce a ogni tessera tolta: tilemapLayer_draw ricalcola le sue righe
TilemapLayer *tilemap_createLayer(Tilemap *m, const char *name, double depth) {
    Tile *data = NULL;
    for (int i = 0; i < m->n_layers; i++) if (!strcmp(m->layerName[i], name)) data = m->layerData[i];
    if (!data) {
        cJSON *lj = tilemap_layerJSON(m, name), *d = cJSON_GetObjectItem(lj, "data");
        if (!d || m->n_layers >= 16) return NULL;
        data = calloc(m->width * m->height, sizeof(Tile));
        int i = 0;
        cJSON *g;
        cJSON_ArrayForEach(g, d) if (i < m->width * m->height) ParseGID((unsigned)cJSON_GetNumberValue(g), &data[i++]);
        m->layerName[m->n_layers] = cJSON_GetStringValue(cJSON_GetObjectItem(lj, "name"));
        m->layerData[m->n_layers++] = data;
    }
    TilemapLayer *l = gmem_calloc(1, sizeof(TilemapLayer));
    sprite_init(&l->s, NULL, 0, 0);
    l->s.owner = l;
    l->s.draw = tilemapLayer_draw;
    l->s.depth = depth;
    l->map = m;
    l->data = data;
    l->tint = 0xffffff;
    children_add(&l->s);
    return l;
}

// SetCollisionByProperty({ prop: true }) + CalculateFacesWithin sull'intero layer
void tilemapLayer_setCollisionByProperty(TilemapLayer *l, const char *prop) {
    Tilemap *m = l->map;
    cJSON *tiles = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(m->json, "tilesets"), 0), "tiles");
    for (int i = 0; i < m->width * m->height; i++) {
        Tile *t = &l->data[i];
        if (t->index < 0) continue;
        cJSON *td;
        cJSON_ArrayForEach(td, tiles) {
            if (cJSON_GetObjectItem(td, "id")->valueint != t->index - m->firstgid) continue;
            cJSON *p;
            cJSON_ArrayForEach(p, cJSON_GetObjectItem(td, "properties"))
                if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(p, "name")), prop) && cJSON_IsTrue(cJSON_GetObjectItem(p, "value"))) t->collides = 1;
        }
    }
    for (int y = 0; y < m->height; y++)
        for (int x = 0; x < m->width; x++) {
            Tile *t = &l->data[y * m->width + x];
            if (!t->collides) { t->faceTop = t->faceBottom = t->faceLeft = t->faceRight = 0; continue; }
            t->faceTop = !(y > 0 && l->data[(y - 1) * m->width + x].collides);
            t->faceBottom = !(y < m->height - 1 && l->data[(y + 1) * m->width + x].collides);
            t->faceLeft = !(x > 0 && l->data[y * m->width + x - 1].collides);
            t->faceRight = !(x < m->width - 1 && l->data[y * m->width + x + 1].collides);
        }
}

static void tile_calculateFacesAt(Tilemap *m, Tile *d, int x, int y) {  // CalculateFacesAt
    if (x < 0 || y < 0 || x >= m->width || y >= m->height) return;
    Tile *t = &d[y * m->width + x];
    if (!t->collides) { t->faceTop = t->faceBottom = t->faceLeft = t->faceRight = 0; return; }
    t->faceTop = !(y > 0 && d[(y - 1) * m->width + x].collides);
    t->faceBottom = !(y < m->height - 1 && d[(y + 1) * m->width + x].collides);
    t->faceLeft = !(x > 0 && d[y * m->width + x - 1].collides);
    t->faceRight = !(x < m->width - 1 && d[y * m->width + x + 1].collides);
}
void tilemapLayer_removeTileAt(TilemapLayer *l, int tx, int ty) {
    Tilemap *m = l->map;
    if (tx < 0 || ty < 0 || tx >= m->width || ty >= m->height) return;
    l->data[ty * m->width + tx] = (Tile){ .index = -1 };
    tiles_ver++;
    for (int y = ty - 1; y <= ty + 1; y++) for (int x = tx - 1; x <= tx + 1; x++) tile_calculateFacesAt(m, l->data, x, y);
}
// map.getTilesWithin(x, y, w, h, {}, layer).forEach(map.removeTile) su ogni layer tranne `except` (rettangolo ritagliato alla mappa)
void tilemap_removeTilesWithin(Tilemap *m, const char *except, int x0, int y0, int w, int h) {
    for (int i = 0; i < m->n_layers; i++) {
        if (except && !strcmp(m->layerName[i], except)) continue;
        Tile *d = m->layerData[i];
        for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < m->height; y++)
            for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < m->width; x++) d[y * m->width + x] = (Tile){ .index = -1 };
        tiles_ver++;
        for (int y = y0 - 1; y <= y0 + h && y < m->height; y++)  // recalculateFaces attorno al rettangolo
            for (int x = x0 - 1; x <= x0 + w && x < m->width; x++) tile_calculateFacesAt(m, d, x, y);
    }
}
Tile *tilemapLayer_getTileAtWorldXY(TilemapLayer *l, float wx, float wy) {  // nonNull = false
    Tilemap *m = l->map;
    int tx = (int)floorf((wx - l->x) / m->tileWidth), ty = (int)floorf((wy - l->y) / m->tileHeight);
    if (tx < 0 || ty < 0 || tx >= m->width || ty >= m->height) return NULL;
    Tile *t = &l->data[ty * m->width + tx];
    return t->index < 0 ? NULL : t;
}

// TilemapLayerWebGLRenderer: tile ritagliati dalla camera, ruotati e specchiati attorno al centro
static void tilemapLayer_draw(Sprite *s) {
    TilemapLayer *l = s->owner;
    if (!s->visible || s->alpha <= 0) return;
    Color lc = { (l->tint >> 16) & 255, (l->tint >> 8) & 255, l->tint & 255, (unsigned char)(255 * fminf(1, s->alpha)) };  // alpha: W_SoleSolution FadeOutLayers / RestoreLayers
    Tilemap *m = l->map;
    float tw = m->tileWidth, th = m->tileHeight;
    int x0 = (int)floorf((camera_scrollX - l->x) / tw), y0 = (int)floorf((camera_scrollY - l->y) / th);
    int x1 = (int)ceilf((camera_scrollX + game_width - l->x) / tw), y1 = (int)ceilf((camera_scrollY + game_height - l->y) / th);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > m->width) x1 = m->width;
    if (y1 > m->height) y1 = m->height;
    // solo le colonne fra la prima e l'ultima tessera di ogni riga: 8 layer su 9 della Foresta hanno 0-91 tessere su 4096,
    // e scorrere tutte le celle visibili di 36 layer costava ~1,5 ms a frame sulla Vita
    if (l->rowsVer != tiles_ver) {
        if (!l->rowMin) l->rowMin = gmem_calloc(m->height, sizeof(short)), l->rowMax = gmem_calloc(m->height, sizeof(short));
        for (int ty = 0; ty < m->height; ty++) {
            l->rowMin[ty] = (short)m->width, l->rowMax[ty] = -1;
            for (int tx = 0; tx < m->width; tx++)
                if (l->data[ty * m->width + tx].index >= 0) { if (l->rowMin[ty] > tx) l->rowMin[ty] = (short)tx; l->rowMax[ty] = (short)tx; }
        }
        l->rowsVer = tiles_ver;
    }
    for (int ty = y0; ty < y1; ty++)
        for (int tx = x0 > l->rowMin[ty] ? x0 : l->rowMin[ty], xe = x1 < l->rowMax[ty] + 1 ? x1 : l->rowMax[ty] + 1; tx < xe; tx++) {
            Tile *t = &l->data[ty * m->width + tx];
            if (t->index < 0) continue;
            prof_tiles++;
            int id = t->index - m->firstgid;
            Rectangle src = { m->margin + (id % m->columns) * (tw + m->spacing), m->margin + (id / m->columns) * (th + m->spacing), tw, th };
            if (t->flipX) src.width = -src.width;
            Rectangle dst = { roundf(l->x + tx * tw + tw / 2 - camera_scrollX), roundf(l->y + ty * th + th / 2 - camera_scrollY), tw, th };
            DrawTexturePro(m->tex, src, dst, (Vector2){ tw / 2, th / 2 }, t->rotation * RAD2DEG, lc);
        }
}

// ============================================================================
// src/physics/arcade/World.js
// ============================================================================
#define OVERLAP_BIAS 4
#define MAX_BODIES 8192
static Sprite *bodies[MAX_BODIES];  // world.bodies (Set, ordine di inserimento)
static int n_bodies;
// world.colliders, senza tetto come l'originale. Blocchi mai spostati: Core e le armi tengono i puntatori e un callback puo' aggiungere collider durante run_colliders
static Collider **colliders;
static int cap_colliders;
// physics.overlapCirc(x, y, r, true, false).length: OverlapRect sui corpi attivi, poi CircleToCircle / CircleToRectangle
int physics_overlapCirc(float x, float y, float r) {
    int n = 0;
    for (int i = 0; i < n_bodies; i++) {
        Body *b = &bodies[i]->body;
        if (!b->enable) continue;
        if (b->position.x + b->width < x - r || b->position.x > x + r || b->position.y + b->height < y - r || b->position.y > y + r) continue;
        if (b->isCircle) {
            float dx = b->center.x - x, dy = b->center.y - y;
            if (sqrtf(dx * dx + dy * dy) <= r + b->halfWidth) n++;
            continue;
        }
        float hw = 0.5f * b->width, hh = 0.5f * b->height, cx = fabsf(x - b->position.x - hw), cy = fabsf(y - b->position.y - hh);
        if (cx > hw + r || cy > hh + r) continue;
        if (cx <= hw || cy <= hh) { n++; continue; }
        float ex = cx - hw, ey = cy - hh;
        if (ex * ex + ey * ey <= r * r) n++;
    }
    return n;
}
static int n_colliders;
static float _elapsed;
static int stepsLastFrame;
static const float _frameTime = 1.0f / 60, _frameTimeMS = 1000.0f / 60;

void physics_reset(void) {
    n_bodies = n_colliders = 0;
    _elapsed = 0;
    world_gravityY = 0;
    stepsLastFrame = 0;
}

// scene.physics.add.existing -> World.enableBody
void physics_existing(Sprite *s) {
    if (!s->hasBody) body_create(s);
    if (!s->body.inWorld && n_bodies < MAX_BODIES) {
        bodies[n_bodies++] = s;
        s->body.inWorld = 1;
    }
    s->body.enable = 1;
}

static Collider *add_collider(SpriteList *a, SpriteList *b, ColliderKind k, PairCallback cb, void *ctx, int overlapOnly) {
    if (n_colliders == cap_colliders) {
        int cap = cap_colliders ? cap_colliders * 2 : 64;
        Collider **t = realloc(colliders, cap * sizeof(*t));
        if (t) colliders = t;
        Collider *block = t ? malloc((cap - cap_colliders) * sizeof(Collider)) : NULL;
        if (!block) { fprintf(stderr, "add_collider: memoria esaurita a %d collider\n", n_colliders); return NULL; }
        for (int i = cap_colliders; i < cap; i++) colliders[i] = &block[i - cap_colliders];
        cap_colliders = cap;
    }
    Collider *c = colliders[n_colliders++];
    *c = (Collider){ .object1 = a, .object2 = b, .kind = k, .collideCallback = cb, .ctx = ctx, .overlapOnly = overlapOnly, .active = 1 };
    return c;
}
Collider *physics_addCollider(SpriteList *a, SpriteList *b, PairCallback cb, void *ctx) { return add_collider(a, b, COLLIDE_ARRAYS, cb, ctx, 0); }
Collider *physics_addOverlap(SpriteList *a, SpriteList *b, PairCallback cb, void *ctx) { return add_collider(a, b, COLLIDE_ARRAYS, cb, ctx, 1); }
Collider *physics_addColliderSelf(SpriteList *g, PairCallback cb, void *ctx) { return add_collider(g, g, COLLIDE_GROUP_SELF, cb, ctx, 0); }
Collider *physics_addOverlapSelf(SpriteList *g, PairCallback cb, void *ctx) { return add_collider(g, g, COLLIDE_GROUP_SELF, cb, ctx, 1); }
Collider *physics_addColliderLayer(SpriteList *g, TilemapLayer *l) {
    Collider *c = add_collider(g, NULL, COLLIDE_TILEMAP, NULL, NULL, 0);
    if (c) c->layer = l;
    return c;
}
Collider *physics_addColliderLayerCb(SpriteList *g, TilemapLayer *l, PairCallback cb, void *ctx) {
    Collider *c = add_collider(g, NULL, COLLIDE_TILEMAP, cb, ctx, 0);
    if (c) c->layer = l;
    return c;
}

// GetOverlapX.js
static float GetOverlapX(Body *b1, Body *b2, int overlapOnly, float bias) {
    float overlap = 0, maxOverlap = deltaAbsX(b1) + deltaAbsX(b2) + bias;
    if (b1->_dx == 0 && b2->_dx == 0) {
        b1->embedded = b2->embedded = 1;
    } else if (b1->_dx > b2->_dx) {
        overlap = body_right(b1) - b2->position.x;
        if ((overlap > maxOverlap && !overlapOnly) || !b1->checkCollision.right || !b2->checkCollision.left) overlap = 0;
        else { b1->touching.none = 0; b1->touching.right = 1; b2->touching.none = 0; b2->touching.left = 1; }
    } else if (b1->_dx < b2->_dx) {
        overlap = b1->position.x - b2->width - b2->position.x;
        if ((-overlap > maxOverlap && !overlapOnly) || !b1->checkCollision.left || !b2->checkCollision.right) overlap = 0;
        else { b1->touching.none = 0; b1->touching.left = 1; b2->touching.none = 0; b2->touching.right = 1; }
    }
    b1->overlapX = b2->overlapX = overlap;
    return overlap;
}

// GetOverlapY.js
static float GetOverlapY(Body *b1, Body *b2, int overlapOnly, float bias) {
    float overlap = 0, maxOverlap = deltaAbsY(b1) + deltaAbsY(b2) + bias;
    if (b1->_dy == 0 && b2->_dy == 0) {
        b1->embedded = b2->embedded = 1;
    } else if (b1->_dy > b2->_dy) {
        overlap = body_bottom(b1) - b2->position.y;
        if ((overlap > maxOverlap && !overlapOnly) || !b1->checkCollision.down || !b2->checkCollision.up) overlap = 0;
        else { b1->touching.none = 0; b1->touching.down = 1; b2->touching.none = 0; b2->touching.up = 1; }
    } else if (b1->_dy < b2->_dy) {
        overlap = b1->position.y - b2->height - b2->position.y;
        if ((-overlap > maxOverlap && !overlapOnly) || !b1->checkCollision.up || !b2->checkCollision.down) overlap = 0;
        else { b1->touching.none = 0; b1->touching.up = 1; b2->touching.none = 0; b2->touching.down = 1; }
    }
    b1->overlapY = b2->overlapY = overlap;
    return overlap;
}

// SeparateX.js / SeparateY.js. ProcessX/ProcessY (separazione reale di due
// rettangoli) servono solo contro muri e tile: il gioco qui non li incontra.
static int SeparateX(Body *b1, Body *b2, int overlapOnly) {
    float overlap = GetOverlapX(b1, b2, overlapOnly, OVERLAP_BIAS);
    if (overlapOnly || overlap == 0 || (b1->immovable && b2->immovable) || b1->customSeparateX || b2->customSeparateX)
        return overlap != 0 || (b1->embedded && b2->embedded);
    // semplificazione: ProcessX non tradotto, nessuna coppia rettangolo-rettangolo solida finora; tradurlo con i muri (F1, mappe con collisioni)
    return 1;
}
static int SeparateY(Body *b1, Body *b2, int overlapOnly) {
    float overlap = GetOverlapY(b1, b2, overlapOnly, OVERLAP_BIAS);
    if (overlapOnly || overlap == 0 || (b1->immovable && b2->immovable) || b1->customSeparateY || b2->customSeparateY)
        return overlap != 0 || (b1->embedded && b2->embedded);
    return 1;
}

// World.circleBodyIntersects
static int circleBodyIntersects(Body *circle, Body *body) {
    float x = clampf(circle->center.x, body->position.x, body_right(body));
    float y = clampf(circle->center.y, body->position.y, body_bottom(body));
    float dx = (circle->center.x - x) * (circle->center.x - x), dy = (circle->center.y - y) * (circle->center.y - y);
    return dx + dy <= circle->halfWidth * circle->halfWidth;
}

// World.intersects
static int intersects(Body *b1, Body *b2) {
    if (b1 == b2) return 0;
    if (!b1->isCircle && !b2->isCircle)
        return !(body_right(b1) <= b2->position.x || body_bottom(b1) <= b2->position.y || b1->position.x >= body_right(b2) || b1->position.y >= body_bottom(b2));
    if (b1->isCircle) {
        if (b2->isCircle) {  // squared: no sqrtf for each pair of the crowd (both sides are >= 0)
            float dx = b1->center.x - b2->center.x, dy = b1->center.y - b2->center.y, r = b1->halfWidth + b2->halfWidth;
            return dx * dx + dy * dy <= r * r;
        }
        return circleBodyIntersects(b1, b2);
    }
    return circleBodyIntersects(b2, b1);
}

// World.separateCircle
static int separateCircle(Body *b1, Body *b2, int overlapOnly) {
    GetOverlapX(b1, b2, 0, 0);
    GetOverlapY(b1, b2, 0, 0);
    float overlap = 0;
    if (b1->isCircle != b2->isCircle) {
        Body *r = b2->isCircle ? b1 : b2, *c = b1->isCircle ? b1 : b2;
        float rx = r->position.x, ry = r->position.y, rr = body_right(r), rb = body_bottom(r);
        float cx = c->center.x, cy = c->center.y, radius = c->halfWidth;
        if (cy < ry) {
            if (cx < rx) overlap = dist(cx, cy, rx, ry) - radius;
            else if (cx > rr) overlap = dist(cx, cy, rr, ry) - radius;
        } else if (cy > rb) {
            if (cx < rx) overlap = dist(cx, cy, rx, rb) - radius;
            else if (cx > rr) overlap = dist(cx, cy, rr, rb) - radius;
        }
        overlap *= -1;
    } else {
        overlap = (b1->halfWidth + b2->halfWidth) - dist(b1->center.x, b1->center.y, b2->center.x, b2->center.y);
    }
    b1->overlapR = b2->overlapR = overlap;
    if (overlapOnly || overlap == 0 || (b1->immovable && b2->immovable) || b1->customSeparateX || b2->customSeparateX)
        return overlap != 0;
    float dx = b1->center.x - b2->center.x, dy = b1->center.y - b2->center.y;
    float d = sqrtf(dx * dx + dy * dy);
    float nx = d ? (b2->center.x - b1->center.x) / d : 0, ny = d ? (b2->center.y - b1->center.y) / d : 0;
    float p = 2 * (b1->velocity.x * nx + b1->velocity.y * ny - b2->velocity.x * nx - b2->velocity.y * ny) / (b1->mass + b2->mass);
    if (b1->immovable || b2->immovable) p *= 2;
    if (!b1->immovable) { b1->velocity.x -= p / b1->mass * nx; b1->velocity.y -= p / b1->mass * ny; }
    if (!b2->immovable) { b2->velocity.x += p / b2->mass * nx; b2->velocity.y += p / b2->mass * ny; }
    if (!b1->immovable && !b2->immovable) overlap /= 2;
    // AngleBetweenPoints + cos/sin: e' la direzione (nx, ny) gia' calcolata (atan2(0, 0) = 0 -> (1, 0)). Sulla Vita
    // atan2f/cosf/sinf per ogni coppia della folla di nemici costavano parecchio.
    float cx = d ? nx : 1, sy = d ? ny : 0;
    float overlapX = (overlap + 1.0e-6f) * cx, overlapY = (overlap + 1.0e-6f) * sy;
    if (!b1->immovable) { b1->position.x -= overlapX; b1->position.y -= overlapY; body_updateCenter(b1); }
    if (!b2->immovable) { b2->position.x += overlapX; b2->position.y += overlapY; body_updateCenter(b2); }
    b1->velocity.x *= b1->bounce.x; b1->velocity.y *= b1->bounce.y;
    b2->velocity.x *= b2->bounce.x; b2->velocity.y *= b2->bounce.y;
    return 1;
}

// World.separate (gravita' zero: si separa prima su Y, come l'originale)
static int separate(Body *b1, Body *b2, int overlapOnly) {
    if (!b1->enable || !b2->enable || b1->checkCollision.none || b2->checkCollision.none || !intersects(b1, b2)) return 0;
    if (b1->isCircle && b2->isCircle) return separateCircle(b1, b2, overlapOnly);
    if (b1->isCircle != b2->isCircle) {
        Body *r = b1->isCircle ? b2 : b1, *c = b1->isCircle ? b1 : b2;
        if (c->center.y < r->position.y || c->center.y > body_bottom(r))
            if (c->center.x < r->position.x || c->center.x > body_right(r)) return separateCircle(b1, b2, overlapOnly);
    }
    int rx = 0, ry = 0;
    if (overlapOnly) {
        rx = SeparateX(b1, b2, 1);
        ry = SeparateY(b1, b2, 1);
    } else {
        ry = SeparateY(b1, b2, 0);
        if (intersects(b1, b2)) rx = SeparateX(b1, b2, 0);
    }
    return rx || ry;
}

// Broadphase al posto dell'RTree: griglia a hash (celle senza limiti di mondo).
// Restituisce i corpi il cui rettangolo tocca quello cercato, come tree.search
// (l'ordine dei risultati puo' differire).
#define GRID_CELL 32
#define GRID_BUCKETS 4096
static int grid_head[GRID_BUCKETS], grid_next[MAX_BODIES], grid_stamp[MAX_BODIES], stamp;

static int cell_of(float v) { return (int)floorf(v / GRID_CELL); }
static int bucket(int cx, int cy) { return (int)(((unsigned)cx * 73856093u ^ (unsigned)cy * 19349663u) & (GRID_BUCKETS - 1)); }

static void tree_load(void) {  // tree.clear() + tree.load(bodies)
    for (int i = 0; i < GRID_BUCKETS; i++) grid_head[i] = -1;
    for (int i = n_bodies - 1; i >= 0; i--) {  // all'indietro: ogni lista esce in ordine di inserimento
        Body *b = &bodies[i]->body;
        int k = bucket(cell_of(b->position.x), cell_of(b->position.y));
        grid_next[i] = grid_head[k];
        grid_head[k] = i;
    }
}

// World.collideSpriteVsGroup (ramo useTree)
static void collideSpriteVsGroup(Sprite *sprite, SpriteList *group, Collider *c) {
    Body *bodyA = &sprite->body;
    if (group->n == 0 || !sprite->hasBody || !bodyA->enable || bodyA->checkCollision.none) return;
    // un corpo e' indicizzato dalla cella del suo angolo in alto a sinistra: si allarga la ricerca di
    // una cella verso l'alto e la sinistra (i corpi dei gruppi sono < GRID_CELL; quelli piu' grandi
    // passano dal ramo a coppie)
    int x0 = cell_of(bodyA->position.x) - 1, y0 = cell_of(bodyA->position.y) - 1;
    int x1 = cell_of(body_right(bodyA)), y1 = cell_of(body_bottom(bodyA));
    stamp++;
    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
            for (int j = grid_head[bucket(cx, cy)]; j >= 0; j = grid_next[j]) {
                if (grid_stamp[j] == stamp) continue;
                grid_stamp[j] = stamp;
                Body *bodyB = &bodies[j]->body;
                if (body_right(bodyB) < bodyA->position.x || bodyB->position.x > body_right(bodyA) ||
                    body_bottom(bodyB) < bodyA->position.y || bodyB->position.y > body_bottom(bodyA))
                    continue;  // tree.search: i rettangoli devono toccarsi
                if (bodyA == bodyB || !bodyB->enable || bodyB->checkCollision.none || !list_contains(group, bodies[j])) continue;
                if (separate(bodyA, bodyB, c->overlapOnly) && c->collideCallback)
                    c->collideCallback(c->ctx, sprite, bodies[j]);
            }
}

// Gruppo contro se stesso (i nemici fra loro) con una griglia dei soli membri: nella griglia di tutti i corpi le celle
// dove muoiono i nemici si riempiono di gemme, e ogni nemico le scorreva una per una per poi scartarle (non membri).
// Stesse coppie di collideSpriteVsGroup; l'ordine dei risultati segue il gruppo invece dell'indice dei corpi.
static int gs_head[GRID_BUCKETS], gs_next[MAX_BODIES], gs_stamp[MAX_BODIES], gs_mark;
static void collideGroupSelfGrid(SpriteList *g, Collider *c) {
    for (int i = 0; i < GRID_BUCKETS; i++) gs_head[i] = -1;
    for (int k = g->n - 1; k >= 0; k--) {
        Sprite *s = g->items[k];
        if (!s->hasBody || !s->body.inWorld) { gs_next[k] = -2; continue; }  // come tree_load: solo i corpi nel mondo
        int b = bucket(cell_of(s->body.position.x), cell_of(s->body.position.y));
        gs_next[k] = gs_head[b], gs_head[b] = k;
    }
    for (int i = 0; i < g->n; i++) {
        Sprite *sprite = g->items[i];
        Body *bodyA = &sprite->body;
        if (!sprite->hasBody || !bodyA->enable || bodyA->checkCollision.none) continue;
        int x0 = cell_of(bodyA->position.x) - 1, y0 = cell_of(bodyA->position.y) - 1;
        int x1 = cell_of(body_right(bodyA)), y1 = cell_of(body_bottom(bodyA));
        gs_mark++;
        for (int cy = y0; cy <= y1; cy++)
            for (int cx = x0; cx <= x1; cx++)
                for (int j = gs_head[bucket(cx, cy)]; j >= 0; j = gs_next[j]) {
                    if (gs_stamp[j] == gs_mark) continue;
                    gs_stamp[j] = gs_mark;
                    if (j >= g->n) continue;  // il callback ha accorciato il gruppo
                    Sprite *other = g->items[j];
                    Body *bodyB = &other->body;
                    if (body_right(bodyB) < bodyA->position.x || bodyB->position.x > body_right(bodyA) ||
                        body_bottom(bodyB) < bodyA->position.y || bodyB->position.y > body_bottom(bodyA))
                        continue;
                    if (bodyA == bodyB || !bodyB->enable || bodyB->checkCollision.none) continue;
                    if (separate(bodyA, bodyB, c->overlapOnly) && c->collideCallback) c->collideCallback(c->ctx, sprite, other);
                }
    }
}

// src/physics/arcade/tilemap: ProcessTileSeparationX/Y, TileCheckX/Y, TileIntersectsBody, SeparateTile
#define TILE_BIAS 16
typedef struct { float left, right, top, bottom; } TileRect;
static void ProcessTileSeparationX(Body *b, float x) {
    if (x < 0) b->blocked.none = 0, b->blocked.left = 1;
    else if (x > 0) b->blocked.none = 0, b->blocked.right = 1;
    b->position.x -= x;
    b->velocity.x = b->bounce.x == 0 ? 0 : -b->velocity.x * b->bounce.x;
}
static void ProcessTileSeparationY(Body *b, float y) {
    if (y < 0) b->blocked.none = 0, b->blocked.up = 1;
    else if (y > 0) b->blocked.none = 0, b->blocked.down = 1;
    b->position.y -= y;
    b->velocity.y = b->bounce.y == 0 ? 0 : -b->velocity.y * b->bounce.y;
}
// collideLeft/Right/Up/Down: true per ogni tile con setCollision (SetTileCollision)
static float TileCheckX(Body *b, Tile *t, float tileLeft, float tileRight) {
    float ox = 0;
    if (b->_dx < 0 && t->collides && b->checkCollision.left) {
        if (t->faceRight && b->position.x < tileRight && (ox = b->position.x - tileRight) < -TILE_BIAS) ox = 0;
    } else if (b->_dx > 0 && t->collides && b->checkCollision.right) {
        if (t->faceLeft && body_right(b) > tileLeft && (ox = body_right(b) - tileLeft) > TILE_BIAS) ox = 0;
    }
    if (ox != 0) {
        if (b->customSeparateX) b->overlapX = ox;
        else ProcessTileSeparationX(b, ox);
    }
    return ox;
}
static float TileCheckY(Body *b, Tile *t, float tileTop, float tileBottom) {
    float oy = 0;
    if (b->_dy < 0 && t->collides && b->checkCollision.up) {
        if (t->faceBottom && b->position.y < tileBottom && (oy = b->position.y - tileBottom) < -TILE_BIAS) oy = 0;
    } else if (b->_dy > 0 && t->collides && b->checkCollision.down) {
        if (t->faceTop && body_bottom(b) > tileTop && (oy = body_bottom(b) - tileTop) > TILE_BIAS) oy = 0;
    }
    if (oy != 0) {
        if (b->customSeparateY) b->overlapY = oy;
        else ProcessTileSeparationY(b, oy);
    }
    return oy;
}
static int TileIntersectsBody(TileRect r, Body *b) {
    return !(body_right(b) <= r.left || body_bottom(b) <= r.top || b->position.x >= r.right || b->position.y >= r.bottom);
}
static int SeparateTile(Body *b, Tile *t, TileRect r) {  // isLayer = true
    int faceH = t->faceLeft || t->faceRight, faceV = t->faceTop || t->faceBottom;
    if (!faceH && !faceV) return 0;
    float ox = 0, oy = 0, minX = 0, minY = 1;
    if (deltaAbsX(b) > deltaAbsY(b)) minX = -1;
    else if (deltaAbsX(b) < deltaAbsY(b)) minY = -1;
    if (b->_dx != 0 && b->_dy != 0 && faceH && faceV) {
        minX = fminf(fabsf(b->position.x - r.right), fabsf(body_right(b) - r.left));
        minY = fminf(fabsf(b->position.y - r.bottom), fabsf(body_bottom(b) - r.top));
    }
    if (minX < minY) {
        if (faceH && (ox = TileCheckX(b, t, r.left, r.right)) != 0 && !TileIntersectsBody(r, b)) return 1;
        if (faceV) oy = TileCheckY(b, t, r.top, r.bottom);
    } else {
        if (faceV && (oy = TileCheckY(b, t, r.top, r.bottom)) != 0 && !TileIntersectsBody(r, b)) return 1;
        if (faceH) ox = TileCheckX(b, t, r.left, r.right);
    }
    return ox != 0 || oy != 0;
}
// World.collideSpriteVsTilemapLayer + GetTilesWithinWorldXY + collideSpriteVsTilesHandler
static void collideSpriteVsTilemapLayer(Sprite *s, TilemapLayer *l, Collider *c) {
    Body *b = &s->body;
    if (!s->hasBody || !b->enable || b->checkCollision.none) return;
    Tilemap *m = l->map;
    float tw = m->tileWidth, th = m->tileHeight;
    // fuori dal layer nessuna tessera: si esce prima dei floorf (nella Foresta a riquadri ogni nemico e' su un solo pezzo di 4)
    if (body_right(b) < l->x || b->position.x > l->x + m->width * tw || body_bottom(b) < l->y || b->position.y > l->y + m->height * th) return;
    int xs = (int)floorf((b->position.x - l->x) / tw), ys = (int)floorf((b->position.y - l->y) / th);
    int xe = (int)ceilf((b->position.x + b->width - l->x) / tw), ye = (int)ceilf((b->position.y + b->height - l->y) / th);
    if (xs < 0) xs = 0;
    if (ys < 0) ys = 0;
    if (xe > m->width) xe = m->width;
    if (ye > m->height) ye = m->height;
    for (int ty = ys; ty < ye; ty++)
        for (int tx = xs; tx < xe; tx++) {
            Tile *t = &l->data[ty * m->width + tx];
            TileRect r = { l->x + tx * tw, l->x + tx * tw + tw, l->y + ty * th, l->y + ty * th + th };
            if (TileIntersectsBody(r, b) && SeparateTile(b, t, r) && c->collideCallback) c->collideCallback(c->ctx, s, (Sprite *)(void *)t);  // la tile al posto del secondo oggetto; ProcessTileCallbacks: nessuno
        }
}

// Grid of the members of one list, for array colliders (each bullet against all enemies). A member goes into every cell
// that its body touches, so bodies of all sizes are found (the grids above assume bodies smaller than one cell).
// The grid only selects candidates: separate() still decides each pair, and the pairs come out in list order.
#define LG_ENTRIES 32768
#define LG_MAX_CELLS 256  // a body that covers more cells uses the loop over all pairs
static int lg_head[GRID_BUCKETS], lg_next[LG_ENTRIES], lg_item[LG_ENTRIES], lg_n, lg_count, lg_stamp[MAX_BODIES], lg_mark;
static Sprite *lg_spr[MAX_BODIES];  // the members when the grid was made (a callback can change the list)
static const SpriteList *lg_list;
static unsigned lg_ver, world_ver;  // world_ver changes when bodies can move: a new step, a collider that separates

static int body_cells(const Body *b, int *x0, int *y0, int *x1, int *y1) {
    *x0 = cell_of(b->position.x), *y0 = cell_of(b->position.y), *x1 = cell_of(body_right(b)), *y1 = cell_of(body_bottom(b));
    return (*x1 - *x0 + 1) * (*y1 - *y0 + 1);
}
static int lg_build(SpriteList *l) {
    if (lg_list == l && lg_ver == world_ver) return 1;
    lg_list = NULL;
    if (l->n > MAX_BODIES) return 0;
    for (int i = 0; i < GRID_BUCKETS; i++) lg_head[i] = -1;
    lg_n = 0;
    for (int j = 0; j < l->n; j++) {
        Sprite *s = lg_spr[j] = l->items[j];
        if (!s->hasBody) continue;
        int x0, y0, x1, y1;
        if (body_cells(&s->body, &x0, &y0, &x1, &y1) > LG_MAX_CELLS) return 0;
        for (int cy = y0; cy <= y1; cy++)
            for (int cx = x0; cx <= x1; cx++) {
                if (lg_n == LG_ENTRIES) return 0;
                int k = bucket(cx, cy);
                lg_item[lg_n] = j, lg_next[lg_n] = lg_head[k], lg_head[k] = lg_n++;
            }
    }
    for (int j = 0; j < l->n; j++) lg_stamp[j] = 0;
    lg_mark = 0, lg_count = l->n;
    lg_list = l, lg_ver = world_ver;
    return 1;
}
// Members whose box touches the box of a (a superset of the pairs that intersect), index > after, in list order.
// Returns -1 if a is too large for a grid search.
static int lg_query(const Body *a, int after, int *out, int max) {
    int x0, y0, x1, y1, n = 0;
    if (body_cells(a, &x0, &y0, &x1, &y1) > LG_MAX_CELLS) return -1;
    lg_mark++;
    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
            for (int e = lg_head[bucket(cx, cy)]; e >= 0; e = lg_next[e]) {
                int j = lg_item[e];
                if (j <= after || lg_stamp[j] == lg_mark) continue;
                lg_stamp[j] = lg_mark;
                const Body *b = &lg_spr[j]->body;
                if (body_right(b) < a->position.x || b->position.x > body_right(a) || body_bottom(b) < a->position.y || b->position.y > body_bottom(a)) continue;
                if (n == max) return -1;
                int k = n++;
                while (k > 0 && out[k - 1] > j) out[k] = out[k - 1], k--;  // insertion sort: list order
                out[k] = j;
            }
    return n;
}
// The loop over all pairs, with the grid of list b. Same pairs in the same order as collider_update's loop below,
// including its behavior when a callback removes the current sprite of list a: the next sprite of a continues
// with the members of b after the current one.
static int collide_arrays_grid(Collider *c) {
    SpriteList *a = c->object1, *b = c->object2;
    if (!c->overlapOnly || !b->bit || a->n < 2 || a->n * b->n < 128 || !lg_build(b)) return 0;
    static int cand[MAX_BODIES];
    for (int i = 0; i < a->n; i++) {
        int after = -1;
        for (;;) {
            Sprite *s1 = a->items[i];
            int n = s1->hasBody ? lg_query(&s1->body, after, cand, MAX_BODIES) : 0, moved = 0;
            if (n < 0) {  // too large: every member after 'after'
                n = 0;
                for (int j = after + 1; j < lg_count; j++) cand[n++] = j;
            }
            for (int k = 0; k < n; k++) {
                Sprite *s2 = lg_spr[cand[k]];
                if (!(s2->groups & b->bit) || !s2->hasBody) continue;  // left the list in a callback
                if (separate(&s1->body, &s2->body, 1) && c->collideCallback) c->collideCallback(c->ctx, s1, s2);
                if (i >= a->n) return 1;
                if (a->items[i] != s1) { after = cand[k], moved = 1; break; }
            }
            if (!moved) break;
        }
    }
    return 1;
}

// World.collideObjects: array contro array (Group senza physicsType = children.entries)
static void collider_update(Collider *c) {
    if (c->kind == COLLIDE_TILEMAP) {  // collideGroupVsTilemapLayer / collideSpriteVsTilemapLayer
        for (int i = 0; i < c->object1->n; i++) {
            int n0 = c->object1->n;
            collideSpriteVsTilemapLayer(c->object1->items[i], c->layer, c);
            if (c->object1->n < n0) i -= n0 - c->object1->n;  // il callback ha tolto lo sprite dal gruppo
        }
        return;
    }
    if (c->kind == COLLIDE_GROUP_SELF) {  // collideGroupVsGroup(g, g): ogni figlio contro il gruppo
        SpriteList *g = c->object1;
        if (g->n <= MAX_BODIES) { collideGroupSelfGrid(g, c); return; }
        tree_load();  // only this path reads the grid of all bodies
        for (int i = 0; i < g->n; i++) collideSpriteVsGroup(g->items[i], g, c);
        return;
    }
    if (collide_arrays_grid(c)) return;
    SpriteList *a = c->object1, *b = c->object2;
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++) {
            Sprite *s1 = a->items[i], *s2 = b->items[j];
            if (!s1->hasBody || !s2->hasBody) continue;
            if (separate(&s1->body, &s2->body, c->overlapOnly) && c->collideCallback) c->collideCallback(c->ctx, s1, s2);
            if (i >= a->n || j >= b->n) break;  // il callback puo' aver tolto oggetti
        }
}

#include <time.h>
static double cp_t[64]; static int cp_n, cp_na[64], cp_nb[64];
static double cp_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
int phys_runs;  // VS_LOGPROF: passaggi di run_colliders (uno per passo di fisica)
static void run_colliders(void) {
    phys_runs++;
    static int on = -1; if (on < 0) on = getenv("VS_COLPROF") != NULL;
    world_ver++;  // the bodies moved in this step: grids from the last step are old
    for (int i = 0; i < n_colliders; i++)
        if (colliders[i]->active) {
            Collider *c = colliders[i];
            double t = on ? cp_now() : 0;
            collider_update(c);
            if (!c->overlapOnly || c->kind != COLLIDE_ARRAYS) world_ver++;  // it can separate (move) bodies
            if (on && i < 64) cp_t[i] += cp_now() - t, cp_na[i] = c->object1 ? c->object1->n : 0, cp_nb[i] = c->object2 ? c->object2->n : 0;
        }
    if (on && ++cp_n == 600) {
#ifdef __vita__
        FILE *o = fopen("ux0:data/VampireSurvivors/colprof.txt", "a");
        if (!o) o = stderr;
#else
        FILE *o = stderr;
#endif
        for (int i = 0; i < n_colliders && i < 64; i++) if (cp_t[i] / cp_n > 0.002) fprintf(o, "COLPROF #%d kind %d %dx%d %.3f ms/chiamata\n", i, colliders[i]->kind, cp_na[i], cp_nb[i], cp_t[i] / cp_n);
        if (o != stderr) fclose(o);
        memset(cp_t, 0, sizeof(cp_t)), cp_n = 0;
    }
}

// World.step
static void world_step(float delta) {
    for (int i = 0; i < n_bodies; i++)
        if (bodies[i]->body.enable) body_update(&bodies[i]->body, delta);
    run_colliders();
    stepsLastFrame++;
}

// World.update (fixedStep a 60 fps)
void world_update(float delta) {
    if (n_bodies == 0) return;
    float fixedDelta = _frameTime, msPerFrame = _frameTimeMS;
    _elapsed += delta;
    int willStep = _elapsed >= msPerFrame;
    for (int i = 0; i < n_bodies; i++)
        if (bodies[i]->body.enable) body_preUpdate(&bodies[i]->body, willStep, fixedDelta);
    if (willStep) {
        _elapsed -= msPerFrame;
        stepsLastFrame = 1;
        run_colliders();
    }
    while (_elapsed >= msPerFrame) {
        _elapsed -= msPerFrame;
        world_step(fixedDelta);
    }
}

// World.postUpdate
void world_postUpdate(void) {
    if (!stepsLastFrame) return;
    stepsLastFrame = 0;
    for (int i = 0; i < n_bodies; i++)
        if (bodies[i]->body.enable) body_postUpdate(&bodies[i]->body);
}

// ============================================================================
// Liste (Group / Array) e lista di disegno
// ============================================================================
int list_contains(const SpriteList *l, const Sprite *s) { return l->bit ? (s->groups & l->bit) != 0 : list_index(l, s) >= 0; }

void list_add(SpriteList *l, Sprite *s) {
    if (list_contains(l, s)) return;
    s->groups |= l->bit;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = gmem_realloc(l->items, l->cap * sizeof(Sprite *));
    }
    l->items[l->n++] = s;
}
int list_index(const SpriteList *l, const Sprite *s) {
    for (int i = 0; i < l->n; i++)
        if (l->items[i] == s) return i;
    return -1;
}
void list_remove(SpriteList *l, Sprite *s) {
    int i = list_index(l, s);
    if (i < 0) return;
    s->groups &= ~l->bit;
    memmove(&l->items[i], &l->items[i + 1], (l->n - i - 1) * sizeof(Sprite *));
    l->n--;
}
void list_free(SpriteList *l) { gmem_free(l->items); *l = (SpriteList){ 0 }; }

static SpriteList children;  // scene.children + updateList
int prof_children, prof_custom, prof_offscreen, prof_breaks, prof_tiles;  // VS_PROF, ultimo scene_render
float render_zoom = 1;  // scene_render: the camera zoom when it is below 1 (zoom-out), for the visible area
static float cullL, cullT, cullR, cullB;  // visible area in view coordinates while scene_render runs (cullOn)
static int cullOn;
// The draw order of the last frame, with the insertion number of each sprite (seq). It changes together with
// 'children': same sprites, and seq increases in the order of the list.
typedef struct { Sprite *s; unsigned seq; } ZEntry;
static ZEntry *zs;
static int zn, zcap;
static unsigned zseq;
static void z_push(Sprite *s) {
    if (zn == zcap) zcap = zcap ? zcap * 2 : 256, zs = realloc(zs, zcap * sizeof(ZEntry));
    zs[zn++] = (ZEntry){ s, ++zseq };
}
static void z_remove(Sprite *s) {
    for (int i = 0; i < zn; i++)
        if (zs[i].s == s) { memmove(&zs[i], &zs[i + 1], (zn - i - 1) * sizeof(ZEntry)); zn--; return; }
}
static void z_rebuild(void) {  // only if the two lists do not agree: new numbers in list order
    zn = 0;
    for (int i = 0; i < children.n; i++) z_push(children.items[i]);
}
void children_add(Sprite *s) {
    if (s->onDisplayList) return;
    int n0 = children.n;
    list_add(&children, s);
    s->onDisplayList = 1;
    if (children.n > n0) z_push(s);
}
void children_remove(Sprite *s) {
    if (!s->onDisplayList) return;
    int n0 = children.n;
    list_remove(&children, s);
    s->onDisplayList = 0;
    if (children.n < n0) z_remove(s);
}
void children_reset(void) { children = (SpriteList){ 0 }; zn = 0; }  // il buffer e' della partita: lo libera gmem_reset

// UpdateList.sceneUpdate -> Sprite.preUpdate -> anims.update
void sprite_animsUpdate(Sprite *s, float delta) { anims_update(s, delta); }  // figli di un Container (fuori dalla lista)

void scene_update_list(float delta) {
    for (int i = 0; i < children.n; i++)
        if (children.items[i]->active) anims_update(children.items[i], delta);
}

// ============================================================================
// Camera (Cameras.Scene2D.Camera, startFollow lerp 1, roundPixels) e disegno
// ============================================================================
float camera_scrollX, camera_scrollY;
int game_width = 730, game_height = 456;

float camera_followOffsetX;
void camera_follow(const Sprite *t) {  // Camera.preRender con follow
    camera_scrollX = t->x - camera_followOffsetX - game_width * 0.5f;
    camera_scrollY = t->y - game_height * 0.5f;
}


// Pipeline multi di Phaser: quad del fotogramma ritagliato, origine, flip, scala, rotazione
int scene_fillActive;  // scene_render: lo shader di riempimento e' quello attivo
void scene_render_one(Sprite *s, Shader fill) {
    const Frame *f = s->frame;
    if (!f || !s->visible || s->alpha <= 0) return;
    float dox = s->originX * s->width, doy = s->originY * s->height;
    float lx = -dox + f->ox, ly = -doy + f->oy;  // angolo del ritaglio rispetto all'origine
    Rectangle src = { f->x, f->y, f->w, f->h };
    // batchSprite: il flip sposta prima x di -realWidth + 2 displayOriginX (cosi' resta sul posto), poi una scala negativa
    // complessiva specchia attorno all'origine (setScale(1, -1) della porta in Inverse)
    if (s->flipX) lx += 2 * dox - s->width;
    if (s->flipY) ly += 2 * doy - s->height;
    if (s->flipX ^ (s->scaleX < 0)) { lx = -(lx + f->w); src.width = -src.width; }
    if (s->flipY ^ (s->scaleY < 0)) { ly = -(ly + f->h); src.height = -src.height; }
    float sx = fabsf(s->scaleX), sy = fabsf(s->scaleY);
    float cx = s->scrollFactor0 ? 0 : camera_scrollX, cy = s->scrollFactor0 ? 0 : camera_scrollY;
    Rectangle dst = { roundf(s->x - cx), roundf(s->y - cy), f->w * sx, f->h * sy };
    Color c = { (s->tint >> 16) & 255, (s->tint >> 8) & 255, s->tint & 255, (unsigned char)(255 * fminf(1, fmaxf(0, s->alpha))) };  // Phaser: setAlpha limita a 0..1
    // tintFill: lo shader di riempimento riconosce u >= 2 (u + 4) e riempie col colore. Dentro scene_render lo shader
    // resta attivo per tutta la lista: cambiarlo per ogni sprite svuotava il batch due volte, e su vitaGL ogni svuotamento
    // copia l'intero VBO in memoria nuova (quad bianchi e lampi neri con molti nemici colpiti insieme)
    int fillMark = s->tintFill && fill.id, own = fillMark && !scene_fillActive;
    if (fillMark) src.x += 4 * f->tex->width;
    if (own) BeginShaderMode(fill);
    if (s->blendAdd) BeginBlendMode(BLEND_ADDITIVE);
    DrawTexturePro(*f->tex, src, dst, (Vector2){ -lx * sx, -ly * sy }, s->angle, c);
    if (s->blendAdd) EndBlendMode();
    if (own) EndShaderMode();
}

void scene_render(Shader fill) {
    double dp_start = GetTime();
    static Sprite **sorted;
    static int cap;
    if (children.n > cap) { cap = children.n * 2; sorted = realloc(sorted, cap * sizeof(Sprite *)); }
    // Stable order: by depth, and by insertion order for the same depth (key: depth, seq). The key is unique, so every
    // sort gives the same order. The insertion sort starts from the order of the last frame: enemy depths change a
    // little each frame, so the list is almost sorted. (From the insertion order, it cost up to 4.6 ms on the Vita.)
    if (zn != children.n) z_rebuild();
    for (int i = 1; i < zn; i++) {
        ZEntry e = zs[i];
        double d = e.s->depth;
        int j = i - 1;
        while (j >= 0 && (zs[j].s->depth > d || (zs[j].s->depth == d && zs[j].seq > e.seq))) zs[j + 1] = zs[j], j--;
        zs[j + 1] = e;
    }
    for (int i = 0; i < zn; i++) sorted[i] = zs[i].s;
    static int skip_draw = -1;  // VS_SKIPDRAW=tiles,pfx,custom,sprites: categorie da non disegnare (misure sulla Vita)
    if (skip_draw < 0) {
        const char *e = getenv("VS_SKIPDRAW");
        skip_draw = !e ? 0 : (strstr(e, "tiles") ? 1 : 0) | (strstr(e, "pfx") ? 2 : 0) | (strstr(e, "custom") ? 4 : 0) | (strstr(e, "sprites") ? 8 : 0);
    }
    // VS_LOGPROF: tempo per categoria (0 ordinamento, 1 tessere, 2 particelle, 3 custom, 4 sprite), preso al cambio di categoria
    extern double draw_prof[8];
    static int dpon = -1;
    if (dpon < 0) dpon = getenv("VS_LOGPROF") != NULL;
    double dpt = dpon ? GetTime() : 0;
    if (dpon) draw_prof[0] += dpt - dp_start;
    int dpc = 0;
#define DP(c) if (dpon && (c) != dpc) { double t_ = GetTime(); draw_prof[dpc] += t_ - dpt, dpt = t_, dpc = (c); }
    unsigned lastTex = 0;
    int lastAdd = -1;
    prof_children = children.n, prof_custom = prof_offscreen = prof_breaks = prof_tiles = 0;
    // Visible area of the game view. A zoom-out (render_zoom < 1) scales the view around its center and shows more.
    const float zx = 0.5f * game_width * (1 / render_zoom - 1), zy = 0.5f * game_height * (1 / render_zoom - 1);
    const float viewL = -zx, viewT = -zy, viewR = game_width + zx, viewB = game_height + zy;
    cullL = viewL, cullT = viewT, cullR = viewR, cullB = viewB, cullOn = 1;  // also for manager_draw, during this pass only
    if (fill.id) BeginShaderMode(fill), scene_fillActive = 1;  // senza segno (u < 2) e' lo shader di default
    for (int i = 0; i < children.n; i++) {
        Sprite *s = sorted[i];
        DP(s->draw ? (s->draw == tilemapLayer_draw ? 1 : s->draw == manager_draw ? 2 : 3) : 4);
        if (s->draw) {
            prof_custom++;
            if (skip_draw & (s->draw == tilemapLayer_draw ? 1 : s->draw == manager_draw ? 2 : 4)) continue;
            s->draw(s);
            if (fill.id) BeginShaderMode(fill);  // chi ha chiuso il suo shader (o la texture ripetuta) torna qui; no-op se gia' attivo
            lastTex = 0;
            continue;
        }
        if (skip_draw & 8) continue;
        if (s->frame && s->visible && s->alpha > 0) {  // VS_PROF: fuori schermo e interruzioni del batch (texture o miscela)
            const Frame *f = s->frame;
            // r: the farthest point of the frame from its origin, with rotation (diagonal <= 1.415 x the longer side)
            float cx = s->scrollFactor0 ? 0 : camera_scrollX, cy = s->scrollFactor0 ? 0 : camera_scrollY, x = s->x - cx, y = s->y - cy,
                  r = 1.5f * fmaxf(f->sw * fabsf(s->scaleX), f->sh * fabsf(s->scaleY));
            if (x < viewL - r || y < viewT - r || x > viewR + r || y > viewB + r) { prof_offscreen++; continue; }  // not visible: no quad
            if (f->tex->id != lastTex || s->blendAdd != lastAdd || s->tintFill) prof_breaks++;
            lastTex = f->tex->id, lastAdd = s->blendAdd;
        }
        scene_render_one(s, fill);
    }
    if (fill.id) EndShaderMode(), scene_fillActive = 0;
    cullOn = 0;
    DP(5);  // chiude l'ultima categoria (5 resta vuota qui: in game_draw e' il resto del disegno)
#undef DP
}

// ============================================================================
// src/gameobjects/particles (ParticleEmitterManager, ParticleEmitter, Particle)
// ============================================================================
static float op_emit(EmitterOp o) { return o.kind == 1 ? o.a + (o.b - o.a) * ((float)rand() / ((float)RAND_MAX + 1.0f)) : o.a; }
static float frnd(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }

static void manager_preUpdate(ParticleManager *m, float delta);
static void manager_draw(Sprite *s);

ParticleManager *particles_add(void) {
    ParticleManager *m = gmem_calloc(1, sizeof(ParticleManager));
    sprite_init(&m->s, NULL, 0, 0);
    m->s.owner = m;
    m->s.draw = manager_draw;
    children_add(&m->s);
    return m;
}

ParticleEmitter *particles_createEmitter(ParticleManager *m, EmitterConfig c) {
    if (m->nemit >= 8) return NULL;
    ParticleEmitter *e = gmem_calloc(1, sizeof(ParticleEmitter));
    if (c.lifespan.kind == 0 && c.lifespan.a == 0) c.lifespan = EOP(1000);
    if (c.angle.kind == 0 && c.angle.a == 0 && c.angle.b == 0) c.angle = ERND(0, 360);  // default {min:0, max:360}
    if (c.scale.kind == 0 && c.scale.a == 0) c.scale = EOP(1);
    if (c.alpha.kind == 0 && c.alpha.a == 0) c.alpha = EOP(1);
    if (!c.quantity) c.quantity = 1;
    if (!c.tint) c.tint = 0xffffff;
    e->c = c;
    e->stepCounter = c.angle.a;
    m->emitters[m->nemit++] = e;
    return e;
}

void emitter_start(ParticleEmitter *e) { e->c.on = 1; e->counter = 0; }
void emitter_stop(ParticleEmitter *e) { e->c.on = 0; }

// Particle.fire
static void particle_fire(ParticleEmitter *e, float x, float y) {
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 32; e->alive = gmem_realloc(e->alive, e->cap * sizeof(Particle)); }
    Particle *p = &e->alive[e->n++];
    *p = (Particle){ 0 };
    p->frame = !e->c.nframes ? NULL : e->c.frameCycle ? e->c.frames[e->frameIndex++ % e->c.nframes] : e->c.frames[(int)floorf(frnd() * e->c.nframes)];
    if (e->c.nEdge) p->x = e->c.edge[e->edgeIndex % e->c.nEdge].x, p->y = e->c.edge[e->edgeIndex % e->c.nEdge].y, e->edgeIndex++;
    if (e->c.hasZone) {  // RandomZone su un cerchio: Circle.Random
        float t = 2 * PI * frnd(), u = frnd() + frnd(), r = u > 1 ? 2 - u : u;
        p->x = r * cosf(t) * e->c.zoneRadius;
        p->y = r * sinf(t) * e->c.zoneRadius;
    }
    if (e->c.hasZoneRect) p->x = e->c.zoneRect.x + frnd() * e->c.zoneRect.width, p->y = e->c.zoneRect.y + frnd() * e->c.zoneRect.height;
    p->x += x + op_emit(e->c.xOffset);
    p->y += y;
    p->life = p->lifeCurrent = op_emit(e->c.lifespan);
    float sp = op_emit(e->c.speed);
    if (e->c.nonRadial) {  // emitter.radial = false (speedX / speedY nella config)
        p->vx = sp;
        p->vy = op_emit(e->c.speedY);
    } else {
        float deg;
        if (e->c.angleSteps) {  // steppedEmit: valore corrente, poi counter = Wrap(counter + (end - start) / steps, start, end)
            deg = e->stepCounter;
            float r = e->c.angle.b - e->c.angle.a, n = e->stepCounter + r / e->c.angleSteps - e->c.angle.a;
            e->stepCounter = e->c.angle.a + fmodf(fmodf(n, r) + r, r);
        } else {
            deg = op_emit(e->c.angle);
        }
        float rad = deg * (PI / 180);
        p->vx = cosf(rad) * fabsf(sp);
        p->vy = sinf(rad) * fabsf(sp);
    }
    p->scale = p->scaleStart = op_emit(e->c.scale);
    p->scaleY = e->c.hasScaleY ? op_emit(e->c.scaleY) : p->scale;
    p->tint = e->c.ntints ? e->c.tints[(int)floorf(frnd() * e->c.ntints)] : e->c.tint;
    p->angle = op_emit(e->c.rotate);
    p->alpha = p->alphaStart = op_emit(e->c.alpha);
}

void emitter_emitParticleAt(ParticleEmitter *e, float x, float y, int count) {
    if (count <= 0) count = e->c.quantity;
    for (int i = 0; i < count; i++) particle_fire(e, x, y);
}
void manager_emitParticleAt(ParticleManager *m, float x, float y) {
    for (int i = 0; i < m->nemit; i++) emitter_emitParticleAt(m->emitters[i], x, y, 0);
}

static void manager_preUpdate(ParticleManager *m, float delta) {
    float step = delta / 1000;
    for (int k = 0; k < m->nemit; k++) {
        ParticleEmitter *e = m->emitters[k];
        if (e->c.follow) { e->x = e->c.follow->x; e->y = e->c.follow->y; }
        for (int i = 0; i < e->n; i++) {  // Particle.update
            Particle *p = &e->alive[i];
            float t = 1 - p->lifeCurrent / p->life;
            p->vy += e->c.gravityY * step;
            if (e->c.hasTintEnd) p->tint = (unsigned)(e->c.tint + ((double)e->c.tintEnd - e->c.tint) * t);
            if (m->hasWell) {  // GravityWell.update (processors)
                float dx = m->wellX - p->x, dy = m->wellY - p->y, dSq = dx * dx + dy * dy;
                if (dSq != 0) {
                    float d = sqrtf(dSq);
                    if (dSq < m->wellEpsilon) dSq = m->wellEpsilon;
                    float f = m->wellPower * delta / (dSq * d) * 100;
                    p->vx += dx * f;
                    p->vy += dy * f;
                }
            }
            if (m->hasWell2) {
                float dx = m->well2X - p->x, dy = m->well2Y - p->y, dSq = dx * dx + dy * dy;
                if (dSq != 0) {
                    float d = sqrtf(dSq);
                    if (dSq < m->wellEpsilon) dSq = m->wellEpsilon;
                    float f = m->wellPower * delta / (dSq * d) * 100;
                    p->vx += dx * f;
                    p->vy += dy * f;
                }
            }
            p->x += p->vx * step;
            p->y += p->vy * step;
            if (e->c.collideLeftRight) {  // checkBounds: sinistra / destra
                if (p->x < e->c.boundsX) { p->x = e->c.boundsX; p->vx *= -e->c.bounce; }
                else if (p->x > e->c.boundsX + e->c.boundsW) { p->x = e->c.boundsX + e->c.boundsW; p->vx *= -e->c.bounce; }
            }
            if (e->c.collideTop && p->y < e->boundsY) { p->y = e->boundsY; p->vy *= -e->c.bounce; }  // checkBounds
            else if (e->c.collideBottom && p->y > e->boundsY + e->c.boundsH) { p->y = e->boundsY + e->c.boundsH; p->vy *= -e->c.bounce; }
            if (e->c.scale.kind == 2) p->scale = e->c.scale.a + (e->c.scale.b - e->c.scale.a) * (e->c.scale.ease ? e->c.scale.ease(t) : t);
            p->scaleY = !e->c.hasScaleY ? p->scale : e->c.scaleY.kind == 2 ? e->c.scaleY.a + (e->c.scaleY.b - e->c.scaleY.a) * t : p->scaleY;
            if (e->c.alpha.kind == 2) p->alpha = e->c.alpha.a + (e->c.alpha.b - e->c.alpha.a) * (e->c.alpha.ease ? e->c.alpha.ease(t) : t);
            p->angle += e->c.rotateStep;
            p->lifeCurrent -= delta;
            if (p->lifeCurrent <= 0) { e->alive[i--] = e->alive[--e->n]; }
        }
        if (e->c.on && e->c.flowEveryFrame) emitter_emitParticleAt(e, e->x, e->y, 0);  // frequency 0: ogni frame
        else if (e->c.on && e->c.frequency > 0) {  // flusso continuo
            e->counter -= delta;
            while (e->counter <= 0) { emitter_emitParticleAt(e, e->x, e->y, 0); e->counter += e->c.frequency; }
        }
    }
}

static void manager_draw(Sprite *s) {
    ParticleManager *m = s->owner;
    for (int k = 0; k < m->nemit; k++) {
        ParticleEmitter *e = m->emitters[k];
        if (e->c.blendAdd) BeginBlendMode(BLEND_ADDITIVE);
        for (int i = 0; i < e->n; i++) {
            Particle *p = &e->alive[i];
            const Frame *f = e->c.frameRef ? *e->c.frameRef : p->frame;
            if (!f) continue;
            Color c = { (p->tint >> 16) & 255, (p->tint >> 8) & 255, p->tint & 255, (unsigned char)(255 * fmaxf(0, fminf(1, p->alpha))) };
            float lx = -f->sw * 0.5f + f->ox, ly = -f->sh * 0.5f + f->oy;
            float sx = m->scrollFactor0 ? 0 : camera_scrollX, sy = m->scrollFactor0 ? 0 : camera_scrollY;
            if (cullOn) {  // same test as scene_render: no quad for a particle outside the view
                float x = p->x - sx, y = p->y - sy, r = 1.5f * fmaxf(f->sw * fabsf(p->scale), f->sh * fabsf(p->scaleY));
                if (x < cullL - r || y < cullT - r || x > cullR + r || y > cullB + r) continue;
            }
            Rectangle dst = { roundf(p->x - sx), roundf(p->y - sy), f->w * p->scale, f->h * p->scaleY };
            DrawTexturePro(*f->tex, (Rectangle){ f->x, f->y, f->w, f->h }, dst, (Vector2){ -lx * p->scale, -ly * p->scaleY }, p->angle, c);
        }
        if (e->c.blendAdd) EndBlendMode();
    }
}

int particles_alive(void) {  // VS_PROF
    int n = 0;
    for (int i = 0; i < children.n; i++)
        if (children.items[i]->draw == manager_draw) {
            ParticleManager *m = children.items[i]->owner;
            for (int k = 0; k < m->nemit; k++) n += m->emitters[k]->n;
        }
    return n;
}
void particles_update_all(float delta) {
    for (int i = 0; i < children.n; i++)
        if (children.items[i]->draw == manager_draw) manager_preUpdate(children.items[i]->owner, delta);
}

// ============================================================================
// ArcadePhysics.closest, velocityFromRotation
// ============================================================================
Sprite *physics_closest(const Sprite *from, SpriteList *targets) {
    float best = 1e30f;
    Sprite *r = NULL;
    for (int i = 0; i < targets->n; i++) {
        Sprite *t = targets->items[i];
        Body *b = &t->body;
        float d = (from->x - b->center.x) * (from->x - b->center.x) + (from->y - b->center.y) * (from->y - b->center.y);
        if (d < best) { best = d; r = t; }
    }
    return r;
}

Vector2 velocityFromRotation(float rad, float speed) { return (Vector2){ cosf(rad) * speed, sinf(rad) * speed }; }

void particles_manager_update(ParticleManager *m, float delta) { manager_preUpdate(m, delta); }
void particles_manager_draw(ParticleManager *m) { manager_draw(&m->s); }

void physics_drawDebug(void) {
    for (int i = 0; i < n_bodies; i++) {
        Body *b = &bodies[i]->body;
        if (!b->enable) continue;
        float x = b->position.x - camera_scrollX, y = b->position.y - camera_scrollY;
        if (b->isCircle) DrawCircleLines((int)(b->center.x - camera_scrollX), (int)(b->center.y - camera_scrollY), b->halfWidth, MAGENTA);
        else DrawRectangleLines((int)x, (int)y, (int)b->width, (int)b->height, MAGENTA);
    }
}

// ============================================================================
// rexShatterImage (phaser3-rex-plugins: shatterimage + utils/math/triangulate)
// La Mesh e' ortografica (setOrtho): le coordinate dei vertici sono in unita' di
// altezza della texture, centrate; a schermo vx = x * height, vy = -y * height.
// ============================================================================

// delaunay-fast (ironwallaby), il modulo 66205 del bundle: Bowyer-Watson incrementale
#define DELAUNAY_EPSILON (1.0 / 1048576.0)
typedef struct { int i, j, k; double x, y, r; } DCircle;
typedef struct { int *a; int n, cap; } IntArr;
static void ia_push(IntArr *v, int x) { if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 64; v->a = realloc(v->a, v->cap * sizeof(int)); } v->a[v->n++] = x; }
static int ia_get(const IntArr *v, int i) { return i >= 0 && i < v->n ? v->a[i] : -1; }  // fuori dall'array: undefined
static void ia_splice2(IntArr *v, int i) {  // splice(i, 2)
    if (i >= v->n) return;
    int k = i + 2 > v->n ? v->n - i : 2;
    memmove(&v->a[i], &v->a[i + k], (v->n - i - k) * sizeof(int));
    v->n -= k;
}
static int d_circumcircle(double (*v)[2], int i, int j, int k, DCircle *out) {
    double x1 = v[i][0], y1 = v[i][1], x2 = v[j][0], y2 = v[j][1], x3 = v[k][0], y3 = v[k][1];
    double fabsy1y2 = fabs(y1 - y2), fabsy2y3 = fabs(y2 - y3), xc, yc, m1, m2, mx1, mx2, my1, my2, dx, dy;
    if (fabsy1y2 < DELAUNAY_EPSILON && fabsy2y3 < DELAUNAY_EPSILON) return 0;  // throw new Error("Eek! Coincident points!")
    if (fabsy1y2 < DELAUNAY_EPSILON) {
        m2 = -((x3 - x2) / (y3 - y2));
        mx2 = (x2 + x3) / 2.0;
        my2 = (y2 + y3) / 2.0;
        xc = (x2 + x1) / 2.0;
        yc = m2 * (xc - mx2) + my2;
    } else if (fabsy2y3 < DELAUNAY_EPSILON) {
        m1 = -((x2 - x1) / (y2 - y1));
        mx1 = (x1 + x2) / 2.0;
        my1 = (y1 + y2) / 2.0;
        xc = (x3 + x2) / 2.0;
        yc = m1 * (xc - mx1) + my1;
    } else {
        m1 = -((x2 - x1) / (y2 - y1));
        m2 = -((x3 - x2) / (y3 - y2));
        mx1 = (x1 + x2) / 2.0;
        mx2 = (x2 + x3) / 2.0;
        my1 = (y1 + y2) / 2.0;
        my2 = (y2 + y3) / 2.0;
        xc = (m1 * mx1 - m2 * mx2 + my2 - my1) / (m1 - m2);
        yc = fabsy1y2 > fabsy2y3 ? m1 * (xc - mx1) + my1 : m2 * (xc - mx2) + my2;
    }
    dx = x2 - xc;
    dy = y2 - yc;
    *out = (DCircle){ i, j, k, xc, yc, dx * dx + dy * dy };
    return 1;
}
static void d_dedup(IntArr *e) {
    int a, b, m, n;
    for (int j = e->n; j;) {
        b = ia_get(e, --j);
        a = ia_get(e, --j);
        for (int i = j; i;) {
            n = ia_get(e, --i);
            m = ia_get(e, --i);
            if ((a == m && b == n) || (a == n && b == m)) {
                ia_splice2(e, j);
                ia_splice2(e, i);
                break;
            }
        }
    }
}
// triangulate(vertices): indici dei triangoli (3 per triangolo); 0 se l'originale lancerebbe un'eccezione
static int d_triangulate(double (*pts)[2], int n, IntArr *res) {
    res->n = 0;
    if (n < 3) return 1;
    double (*v)[2] = malloc((n + 3) * sizeof(*v));  // vertices.slice(0)
    memcpy(v, pts, n * sizeof(*v));
    int *idx = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {  // indices.sort((i, j) => vertices[j][0] - vertices[i][0]): stabile
        int k = idx[i], j = i - 1;
        while (j >= 0 && v[k][0] - v[idx[j]][0] > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = k;
    }
    // supertriangle
    double xmin = INFINITY, ymin = INFINITY, xmax = -INFINITY, ymax = -INFINITY;
    for (int i = n; i--;) {
        if (v[i][0] < xmin) xmin = v[i][0];
        if (v[i][0] > xmax) xmax = v[i][0];
        if (v[i][1] < ymin) ymin = v[i][1];
        if (v[i][1] > ymax) ymax = v[i][1];
    }
    double dx = xmax - xmin, dy = ymax - ymin, dmax = fmax(dx, dy), xmid = xmin + dx * 0.5, ymid = ymin + dy * 0.5;
    v[n][0] = xmid - 20 * dmax, v[n][1] = ymid - dmax;
    v[n + 1][0] = xmid, v[n + 1][1] = ymid + 20 * dmax;
    v[n + 2][0] = xmid + 20 * dmax, v[n + 2][1] = ymid - dmax;
    int ok = 1, nopen = 1, ncl = 0, capo = 64, capc = 64;
    DCircle *open = malloc(capo * sizeof(DCircle)), *closed = malloc(capc * sizeof(DCircle));
    IntArr edges = { 0 };
    if (!d_circumcircle(v, n, n + 1, n + 2, &open[0])) { ok = 0; goto done; }
    for (int i = n; i--; edges.n = 0) {
        int c = idx[i];
        for (int j = nopen; j--;) {
            double ddx = v[c][0] - open[j].x;
            if (ddx > 0.0 && ddx * ddx > open[j].r) {
                if (ncl == capc) closed = realloc(closed, (capc *= 2) * sizeof(DCircle));
                closed[ncl++] = open[j];
                memmove(&open[j], &open[j + 1], (nopen - j - 1) * sizeof(DCircle)), nopen--;
                continue;
            }
            double ddy = v[c][1] - open[j].y;
            if (ddx * ddx + ddy * ddy - open[j].r > DELAUNAY_EPSILON) continue;
            ia_push(&edges, open[j].i), ia_push(&edges, open[j].j), ia_push(&edges, open[j].j);
            ia_push(&edges, open[j].k), ia_push(&edges, open[j].k), ia_push(&edges, open[j].i);
            memmove(&open[j], &open[j + 1], (nopen - j - 1) * sizeof(DCircle)), nopen--;
        }
        d_dedup(&edges);
        for (int j = edges.n; j;) {
            int b = edges.a[--j], a = edges.a[--j];
            if (nopen == capo) open = realloc(open, (capo *= 2) * sizeof(DCircle));
            if (!d_circumcircle(v, a, b, c, &open[nopen++])) { ok = 0; goto done; }
        }
    }
    for (int i = nopen; i--;) {
        if (ncl == capc) closed = realloc(closed, (capc *= 2) * sizeof(DCircle));
        closed[ncl++] = open[i];
    }
    for (int i = ncl; i--;)
        if (closed[i].i < n && closed[i].j < n && closed[i].k < n) ia_push(res, closed[i].i), ia_push(res, closed[i].j), ia_push(res, closed[i].k);
done:
    free(v), free(idx), free(open), free(closed), free(edges.a);
    if (!ok) res->n = 0;
    return ok;
}

// ShatterRectangleToTriangles: anelli di punti attorno al centro
static double shatter_randomRange(double a, double b) { return a == b ? a : a + (b - a) * frnd(); }
static int shatter_ring(double (*out)[2], int n0, double cx, double cy, double radius, int amount, double lo, double hi,
                        double l, double r, double t, double b) {
    for (int i = 0; i < amount; i++) {
        double a = (double)i / amount * 2 * PI;
        double x = cx + cos(a) * radius * shatter_randomRange(lo, hi), y = cy + sin(a) * radius * shatter_randomRange(lo, hi);
        out[n0][0] = x < l ? l : x > r ? r : x;  // Clamp
        out[n0][1] = y < t ? t : y > b ? b : y;
        n0++;
    }
    return n0;
}

ShatterImage *shatterImage_add(float x, float y, const Texture2D *tex, float w, float h) {
    ShatterImage *im = calloc(1, sizeof(ShatterImage));
    sprite_init(&im->s, NULL, x, y);
    im->s.owner = im;
    im->tex = tex;
    im->width = w, im->height = h;  // setSizeToFrame
    im->variation = 0.25f;
    im->samplesPerRing = 12;
    // resetImage: GenerateGridVerts dell'immagine intera. La RenderTexture e' vuota a quel punto, quindi invisibile: non tradotto.
    extern void shatter_draw(Sprite *s);
    im->s.draw = shatter_draw;
    children_add(&im->s);  // scene.add.existing
    return im;
}

static int face_cmp_g(const MeshFace *a, const MeshFace *b) { return a->g > b->g ? 1 : a->g < b->g ? -1 : 0; }

void shatterImage_shatter(ShatterImage *im) {
    double cxs = im->width / 2, cys = im->height / 2;  // shatterCenter
    free(im->faces), free(im->vertices);  // clear()
    im->faces = NULL, im->vertices = NULL, im->nfaces = 0;
    if (im->width == 0 || im->height == 0) return;
    double w = im->width, h = im->height, l = 0, r = w, t = 0, b = h;
    double cx = cxs < l ? l : cxs > r ? r : cxs, cy = cys < t ? t : cys > b ? b : cys;
    int spr = im->samplesPerRing, n = 0;
    double (*pts)[2] = malloc((1 + 4 * spr) * sizeof(*pts));
    pts[n][0] = cx, pts[n][1] = cy, n++;
    double rad = fmin(w, h), lo = 1 - im->variation, hi = 1 + im->variation;
    for (int i = 0; i < 3; i++) n = shatter_ring(pts, n, cx, cy, rad * pow(3, i) / 27, spr, lo, hi, l, r, t, b);
    rad = 2 * fmin(w, h);
    n = shatter_ring(pts, n, cx, cy, rad, spr, lo, hi, l, r, t, b);
    IntArr ind = { 0 };
    d_triangulate(pts, n, &ind);
    // vertici: x = px / height - halfW, y = py / height - halfH, uv della RenderTexture (v capovolta)
    double hw = w / h / 2, hh = h / h / 2, u0 = 0, u1 = 1, v0 = 1, v1 = 0;  // isRenderTexture: v0 = frame.v1, v1 = frame.v0
    im->nfaces = ind.n / 3;
    im->vertices = malloc(ind.n * sizeof(MeshVertex));
    im->faces = malloc(im->nfaces * sizeof(MeshFace));
    for (int f = 0; f < im->nfaces; f++) {
        float g = INFINITY;
        for (int k = 0; k < 3; k++) {
            double *p = pts[ind.a[f * 3 + k]];
            double gx = p[0] - cxs, gy = p[1] - cys, gd = gx * gx + gy * gy;  // DistanceSquared dal centro
            if (gd < g) g = gd;
            im->vertices[f * 3 + k] = (MeshVertex){ p[0] / h - hw, -(p[1] / h - hh), u0 + (u1 - u0) * (p[0] / w), v0 + (v1 - v0) * (p[1] / h), 1 };
        }
        im->faces[f] = (MeshFace){ &im->vertices[f * 3], &im->vertices[f * 3 + 1], &im->vertices[f * 3 + 2], 0, g };
    }
    for (int i = 1; i < im->nfaces; i++) {  // faces.sort((a, b) => a.g - b.g): stabile; i vertici restano dove sono
        MeshFace k = im->faces[i];
        int j = i - 1;
        while (j >= 0 && face_cmp_g(&im->faces[j], &k) > 0) { im->faces[j + 1] = im->faces[j]; j--; }
        im->faces[j + 1] = k;
    }
    free(pts), free(ind.a);
}

// Face.getInCenter (locale)
static Vector2 face_inCenter(const MeshFace *f) {
    float x1 = f->vertex1->x, y1 = f->vertex1->y, x2 = f->vertex2->x, y2 = f->vertex2->y, x3 = f->vertex3->x, y3 = f->vertex3->y;
    float d1 = sqrtf((x3 - x2) * (x3 - x2) + (y3 - y2) * (y3 - y2)), d2 = sqrtf((x1 - x3) * (x1 - x3) + (y1 - y3) * (y1 - y3)),
          d3 = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)), p = d1 + d2 + d3;
    return (Vector2){ (x1 * d1 + x2 * d2 + x3 * d3) / p, (y1 * d1 + y2 * d2 + y3 * d3) / p };
}
static void face_translate(MeshFace *f, float x, float y) {
    f->vertex1->x += x, f->vertex1->y += y;
    f->vertex2->x += x, f->vertex2->y += y;
    f->vertex3->x += x, f->vertex3->y += y;
}
static void face_rotate(MeshFace *f, float angle) {  // Geom.Mesh.RotateFace attorno all'incentro
    Vector2 ic = face_inCenter(f);
    float c = cosf(angle), s = sinf(angle);
    MeshVertex *vs[3] = { f->vertex1, f->vertex2, f->vertex3 };
    for (int i = 0; i < 3; i++) {
        float tx = vs[i]->x - ic.x, ty = vs[i]->y - ic.y;
        vs[i]->x = tx * c - ty * s + ic.x;
        vs[i]->y = tx * s + ty * c + ic.y;
    }
}
static float face_getAlpha(void *o) { MeshFace *f = o; return (f->vertex1->alpha + f->vertex2->alpha + f->vertex3->alpha) / 3; }
static void face_setAlpha(void *o, float v) { MeshFace *f = o; f->vertex1->alpha = f->vertex2->alpha = f->vertex3->alpha = v; }
static float face_getAngle(void *o) { return ((MeshFace *)o)->_rotation * (180 / PI); }
static void face_setAngle(void *o, float v) {  // angle -> rotation: RotateFace(face, rotation - _rotation)
    MeshFace *f = o;
    float r = v * (PI / 180);
    face_rotate(f, r - f->_rotation);
    f->_rotation = r;
}
static float face_getX(void *o) { return face_inCenter(o).x; }
static void face_setX(void *o, float v) { face_translate(o, v - face_inCenter(o).x, 0); }
static float face_getY(void *o) { return face_inCenter(o).y; }
static void face_setY(void *o, float v) { face_translate(o, 0, v - face_inCenter(o).y); }
TweenProp face_prop(MeshFace *f, int which, float end) {
    static float (*const G[4])(void *) = { face_getAlpha, face_getAngle, face_getX, face_getY };
    static void (*const S[4])(void *, float) = { face_setAlpha, face_setAngle, face_setX, face_setY };
    return (TweenProp){ .end = end, .get = G[which], .set = S[which], .o = f };
}

// MeshWebGLRenderer + Face.isInView (alpha e area nulla; il controllo sulla camera non cambia l'immagine)
void shatter_draw(Sprite *s) {
    ShatterImage *im = s->owner;
    if (!s->visible || !im->nfaces || !im->tex) return;
    float H = im->height, ox = s->x - (s->scrollFactor0 ? 0 : camera_scrollX), oy = s->y - (s->scrollFactor0 ? 0 : camera_scrollY);
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();  // hideCCW = false
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);  // la RenderTexture e' premoltiplicata come in WebGL
    rlCheckRenderBatchLimit(im->nfaces * 3);
    rlSetTexture(im->tex->id);
    rlBegin(RL_TRIANGLES);
    for (int i = 0; i < im->nfaces; i++) {
        MeshFace *f = &im->faces[i];
        MeshVertex *vs[3] = { f->vertex1, f->vertex2, f->vertex3 };
        float tx[3], ty[3], ta[3];
        for (int k = 0; k < 3; k++) {  // transformCoordinatesLocal (ortho) + Vertex.update (roundPixels)
            tx[k] = roundf(vs[k]->x * H * s->scaleX + ox);
            ty[k] = roundf(-vs[k]->y * H * s->scaleY + oy);
            ta[k] = vs[k]->alpha * s->alpha;
        }
        if (ta[0] <= 0 && ta[1] <= 0 && ta[2] <= 0) continue;
        if (fmaxf(tx[0], fmaxf(tx[1], tx[2])) - fminf(tx[0], fminf(tx[1], tx[2])) <= 0 ||
            fmaxf(ty[0], fmaxf(ty[1], ty[2])) - fminf(ty[0], fminf(ty[1], ty[2])) <= 0) continue;
        for (int k = 0; k < 3; k++) {
            unsigned char a = (unsigned char)(255 * fmaxf(0, fminf(1, ta[k])));  // tint premoltiplicato: (a, a, a, a)
            rlColor4ub(a, a, a, a);
            rlTexCoord2f(vs[k]->u, vs[k]->v);
            rlVertex2f(tx[k], ty[k]);
        }
    }
    rlEnd();
    rlSetTexture(0);
    EndBlendMode();
    rlEnableBackfaceCulling();
}
