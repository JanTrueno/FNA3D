/* FNA3D - 3D Graphics Library for FNA
 *
 * The OpenGL driver's GL calls on a thread of their own (FNA3D_OPENGL_THREAD=1).
 *
 * Everything the driver and MojoShader decide still happens on the game's thread, in the same
 * order; only the calls into the GL driver (and the swap) move. Each call is written into a ring
 * with its arguments (and a copy of any memory it reads), and the GL thread, which holds the
 * context, makes the calls in order. A call that answers (a glGet, a glCreate, a read back)
 * waits until the GL thread has made it and every call before it. The game's thread runs at
 * most one frame ahead: a swap waits for the swap before it.
 *
 * Off by default. Started at the end of OPENGL_CreateDevice (until then every call goes straight
 * to the driver) and stopped first thing in OPENGL_DestroyDevice. Not started for a debug
 * context or a MojoShader profile other than GLSL (the ARB profiles' calls are not wrapped).
 * FNA3D_OPENGL_THREAD_STATS=1 logs what the thread did every 600 frames.
 *
 * Local to the Terraria branch; not part of FNA3D upstream.
 */

#if defined(__GNUC__) || defined(__clang__)
#define GLT_SUPPORTED 1
#define GLT_LOAD(p)	__atomic_load_n(p, __ATOMIC_SEQ_CST)
#define GLT_STORE(p, v)	__atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define GLT_XCHG(p, v)	__atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
#define GLT_ADD(p, v)	__atomic_add_fetch(p, v, __ATOMIC_SEQ_CST)
#define GLT_SUB(p, v)	__atomic_sub_fetch(p, v, __ATOMIC_SEQ_CST)
#if defined(__aarch64__) || defined(__arm__)
#define GLT_RELAX()	__asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define GLT_RELAX()	__builtin_ia32_pause()
#else
#define GLT_RELAX()	__asm__ __volatile__("" ::: "memory")
#endif
#else
/* No atomics we trust here: the thread is never started, the calls go straight through */
#define GLT_SUPPORTED 0
#define GLT_LOAD(p)	(*(p))
#define GLT_STORE(p, v)	(*(p) = (v))
#define GLT_XCHG(p, v)	(*(p) = (v))
#define GLT_ADD(p, v)	(*(p) += (v))
#define GLT_SUB(p, v)	(*(p) -= (v))
#define GLT_RELAX()
#endif

#define GLT_RING_SIZE	(4 * 1024 * 1024)	/* calls and their copied memory */
#define GLT_RING_MASK	(GLT_RING_SIZE - 1)
#define GLT_INLINE_MAX	(256 * 1024)		/* larger copies go to the heap... */
#define GLT_HEAP_MAX	(64 * 1024 * 1024)	/* ...up to this much at a time */
#define GLT_KICK_BYTES	(16 * 1024)		/* a sleeping GL thread is woken after this much */
#define GLT_SPIN	512			/* checks before the GL thread sleeps */
#define GLT_FRAMES	1			/* swaps the game may be ahead of the GL thread */
#define GLT_POOL	64			/* texture and buffer names made ahead */

/* The entry points MojoShader's GLSL profiles use that the driver does not */
typedef GLenum (GLAPIENTRY *glfntype_glGetError)(void);
typedef void (GLAPIENTRY *glfntype_glDeleteShader)(GLuint a);
typedef void (GLAPIENTRY *glfntype_glDeleteProgram)(GLuint a);
typedef void (GLAPIENTRY *glfntype_glAttachShader)(GLuint a, GLuint b);
typedef void (GLAPIENTRY *glfntype_glCompileShader)(GLuint a);
typedef GLuint (GLAPIENTRY *glfntype_glCreateShader)(GLenum a);
typedef GLuint (GLAPIENTRY *glfntype_glCreateProgram)(void);
typedef GLint (GLAPIENTRY *glfntype_glGetAttribLocation)(GLuint a, const GLchar *b);
typedef void (GLAPIENTRY *glfntype_glGetProgramInfoLog)(GLuint a, GLsizei b, GLsizei *c, GLchar *d);
typedef void (GLAPIENTRY *glfntype_glGetShaderInfoLog)(GLuint a, GLsizei b, GLsizei *c, GLchar *d);
typedef void (GLAPIENTRY *glfntype_glGetShaderiv)(GLuint a, GLenum b, GLint *c);
typedef void (GLAPIENTRY *glfntype_glGetProgramiv)(GLuint a, GLenum b, GLint *c);
typedef GLint (GLAPIENTRY *glfntype_glGetUniformLocation)(GLuint a, const GLchar *b);
typedef void (GLAPIENTRY *glfntype_glLinkProgram)(GLuint a);
typedef void (GLAPIENTRY *glfntype_glShaderSource)(GLuint a, GLsizei b, const GLchar* const *c, const GLint *d);
typedef void (GLAPIENTRY *glfntype_glUniform1i)(GLint a, GLint b);
typedef void (GLAPIENTRY *glfntype_glUniform1iv)(GLint a, GLsizei b, const GLint *c);
typedef void (GLAPIENTRY *glfntype_glUniform2f)(GLint a, GLfloat b, GLfloat c);
typedef void (GLAPIENTRY *glfntype_glUniform1f)(GLint a, GLfloat b);
typedef void (GLAPIENTRY *glfntype_glUniform4fv)(GLint a, GLsizei b, const GLfloat *c);
typedef void (GLAPIENTRY *glfntype_glUniform4iv)(GLint a, GLsizei b, const GLint *c);
typedef void (GLAPIENTRY *glfntype_glUseProgram)(GLuint a);
typedef void (GLAPIENTRY *glfntype_glVertexAttribDivisorARB)(GLuint a, GLuint b);

/* The driver's own entry points, called by the GL thread (or directly while it is not running) */
static struct
{
	glfntype_glGetString glGetString;
	#define GL_EXT(ext)
	#define GL_PROC(ext, ret, func, parms) \
		glfntype_##func func;
	#define GL_PROC_EXT(ext, fallback, ret, func, parms) \
		glfntype_##func func;
	#include "FNA3D_Driver_OpenGL_glfuncs.h"
	glfntype_glGetError glGetError;
	glfntype_glDeleteShader glDeleteShader;
	glfntype_glDeleteProgram glDeleteProgram;
	glfntype_glAttachShader glAttachShader;
	glfntype_glCompileShader glCompileShader;
	glfntype_glCreateShader glCreateShader;
	glfntype_glCreateProgram glCreateProgram;
	glfntype_glGetAttribLocation glGetAttribLocation;
	glfntype_glGetProgramInfoLog glGetProgramInfoLog;
	glfntype_glGetShaderInfoLog glGetShaderInfoLog;
	glfntype_glGetShaderiv glGetShaderiv;
	glfntype_glGetProgramiv glGetProgramiv;
	glfntype_glGetUniformLocation glGetUniformLocation;
	glfntype_glLinkProgram glLinkProgram;
	glfntype_glShaderSource glShaderSource;
	glfntype_glUniform1i glUniform1i;
	glfntype_glUniform1iv glUniform1iv;
	glfntype_glUniform2f glUniform2f;
	glfntype_glUniform1f glUniform1f;
	glfntype_glUniform4fv glUniform4fv;
	glfntype_glUniform4iv glUniform4iv;
	glfntype_glUseProgram glUseProgram;
	glfntype_glVertexAttribDivisorARB glVertexAttribDivisorARB;
} glt_real;

typedef void (*GLThreadRun)(void *args);

typedef struct GLThreadCmd
{
	GLThreadRun run;	/* NULL: the rest of the ring is skipped */
	uint64_t size;		/* this header and the arguments, a multiple of 16 */
} GLThreadCmd;
#define GLT_HDR ((sizeof(GLThreadCmd) + 15) & ~((size_t) 15))

static struct
{
	/* Set on the game's thread only */
	uint8_t wanted;
	uint8_t active;
	SDL_Window *window;
	SDL_GLContext context;
	SDL_Thread *thread;
	SDL_Semaphore *wakeGL;
	SDL_Semaphore *wakeGame;
	uint8_t *ring;
	uint64_t written;	/* the game's end of the ring, published as w */
	uint64_t pending;	/* the size of the call being written */
	uint64_t kicked;	/* written, when the GL thread was last woken */
	uint64_t swapsQueued;
	GLint unpackAlignment;
	uint32_t unpackOther;	/* row length, skips, image height: nonzero ones */
	GLuint texturePool[GLT_POOL];
	int32_t texturePoolCount;
	GLuint bufferPool[GLT_POOL];
	int32_t bufferPoolCount;

	/* Shared */
	uint64_t w;		/* published: calls written */
	uint64_t r;		/* published: calls made */
	uint64_t swapsDone;
	uint64_t heapBytes;
	int32_t glSleeping;
	int32_t gameWaiting;
	int32_t started;	/* 1 the GL thread holds the context, -1 it could not */

	/* GL thread only */
	uint8_t quit;

	/* FNA3D_OPENGL_THREAD_STATS */
	uint8_t stats;
	uint64_t statSyncs, statWaits, statWakes, statHeap, statFrames;
	uint64_t statWaitTicks, statSwapWaitTicks, statBytes, statStart;
} glt;

/* The ring */

static inline void GLThread_Kick(void)
{
	glt.kicked = glt.written;
	if (GLT_LOAD(&glt.glSleeping) && GLT_XCHG(&glt.glSleeping, 0))
	{
		glt.statWakes += 1;
		SDL_SignalSemaphore(glt.wakeGL);
	}
}

/* Waits on the game's thread until cond holds; the GL thread signals after every call while
 * the game waits */
#define GLT_WAIT(cond, ticks) \
	if (!(cond)) \
	{ \
		uint64_t waitStart = glt.stats ? SDL_GetPerformanceCounter() : 0; \
		glt.statWaits += 1; \
		for (;;) \
		{ \
			GLThread_Kick(); \
			GLT_STORE(&glt.gameWaiting, 1); \
			if (cond) \
			{ \
				GLT_STORE(&glt.gameWaiting, 0); \
				break; \
			} \
			SDL_WaitSemaphore(glt.wakeGame); \
			if (cond) \
			{ \
				GLT_STORE(&glt.gameWaiting, 0); \
				break; \
			} \
		} \
		if (glt.stats) \
		{ \
			ticks += SDL_GetPerformanceCounter() - waitStart; \
		} \
	}

/* Room for a call with this many bytes of arguments; returns where they go */
static void* GLThread_Begin(GLThreadRun run, size_t argsSize)
{
	uint64_t size = GLT_HDR + ((argsSize + 15) & ~((size_t) 15));
	uint64_t pos = glt.written & GLT_RING_MASK;
	uint64_t rest = GLT_RING_SIZE - pos;
	GLThreadCmd *cmd;

	if (size > rest)
	{
		/* Skip to the ring's start */
		GLT_WAIT(
			glt.written + rest + size - GLT_LOAD(&glt.r) <= GLT_RING_SIZE,
			glt.statWaitTicks
		)
		cmd = (GLThreadCmd*) (glt.ring + pos);
		cmd->run = NULL;
		cmd->size = rest;
		glt.written += rest;
		glt.statBytes += rest;
		pos = 0;
	}
	else
	{
		GLT_WAIT(
			glt.written + size - GLT_LOAD(&glt.r) <= GLT_RING_SIZE,
			glt.statWaitTicks
		)
	}

	cmd = (GLThreadCmd*) (glt.ring + pos);
	cmd->run = run;
	cmd->size = size;
	glt.pending = size;
	return ((uint8_t*) cmd) + GLT_HDR;
}

/* The call is written: the GL thread may make it */
static inline void GLThread_End(uint8_t kick)
{
	glt.written += glt.pending;
	glt.statBytes += glt.pending;
	GLT_STORE(&glt.w, glt.written);
	if (kick || glt.written - glt.kicked >= GLT_KICK_BYTES)
	{
		GLThread_Kick();
	}
}

/* The call is written: wait until the GL thread has made it */
static void GLThread_EndSync(void)
{
	uint64_t target;
	GLThread_End(1);
	target = glt.written;
	glt.statSyncs += 1;
	GLT_WAIT(GLT_LOAD(&glt.r) >= target, glt.statWaitTicks)
}

/* Memory a call reads: copied after its arguments, or to the heap when large. When neither
 * can be had, the call borrows the game's memory and the game waits for it (owned == 2). */
typedef struct GLThreadData
{
	const void *data;
	size_t length;
	uint8_t owned; /* 0 in the ring or none, 1 on the heap, 2 borrowed */
} GLThreadData;

static void* GLThread_BeginData(
	GLThreadRun run,
	size_t argsSize,
	GLThreadData *d,
	const void *src,
	size_t length,
	uint8_t known
) {
	size_t a = (argsSize + 15) & ~((size_t) 15);
	uint8_t *args;
	void *heap;

	d->length = length;
	d->owned = 0;
	if (src == NULL)
	{
		d->data = NULL;
		return GLThread_Begin(run, argsSize);
	}
	if (!known)
	{
		/* A layout we do not size: the GL thread reads the game's memory */
		d->data = src;
		d->owned = 2;
		return GLThread_Begin(run, argsSize);
	}
	if (length <= GLT_INLINE_MAX)
	{
		args = (uint8_t*) GLThread_Begin(run, a + length);
		SDL_memcpy(args + a, src, length);
		d->data = args + a;
		return args;
	}

	GLT_WAIT(
		GLT_LOAD(&glt.heapBytes) == 0 ||
		GLT_LOAD(&glt.heapBytes) + length <= GLT_HEAP_MAX,
		glt.statWaitTicks
	)
	heap = SDL_malloc(length);
	if (heap == NULL)
	{
		d->data = src;
		d->owned = 2;
		return GLThread_Begin(run, argsSize);
	}
	SDL_memcpy(heap, src, length);
	GLT_ADD(&glt.heapBytes, length);
	glt.statHeap += 1;
	d->data = heap;
	d->owned = 1;
	return GLThread_Begin(run, argsSize);
}

static inline void GLThread_EndData(GLThreadData *d)
{
	if (d->owned == 2)
	{
		GLThread_EndSync();
	}
	else
	{
		GLThread_End(0);
	}
}

/* On the GL thread, after the call */
static inline void GLThread_Release(GLThreadData *d)
{
	if (d->owned == 1)
	{
		SDL_free((void*) d->data);
		GLT_SUB(&glt.heapBytes, d->length);
	}
}

/* Calls without memory: their arguments copied. ASYNC returns at once, SYNC waits. */

#define GLT_CALL0(fn, END) \
	static void GLT_##fn##_Run(void *p) \
	{ \
		glt_real.fn(); \
	} \
	static void GLAPIENTRY glt_##fn(void) \
	{ \
		if (!glt.active) { glt_real.fn(); return; } \
		GLThread_Begin(GLT_##fn##_Run, 0); \
		END; \
	}
#define GLT_CALL1(fn, END, A) \
	typedef struct { A a; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a); \
	} \
	static void GLAPIENTRY glt_##fn(A a) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; \
		END; \
	}
#define GLT_CALL2(fn, END, A, B) \
	typedef struct { A a; B b; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; \
		END; \
	}
#define GLT_CALL3(fn, END, A, B, C) \
	typedef struct { A a; B b; C c; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b, x->c); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b, C c) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b, c); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; \
		END; \
	}
#define GLT_CALL4(fn, END, A, B, C, D) \
	typedef struct { A a; B b; C c; D d; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b, x->c, x->d); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b, C c, D d) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b, c, d); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; x->d = d; \
		END; \
	}
#define GLT_CALL5(fn, END, A, B, C, D, E) \
	typedef struct { A a; B b; C c; D d; E e; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b, x->c, x->d, x->e); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b, C c, D d, E e) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b, c, d, e); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; \
		END; \
	}
#define GLT_CALL6(fn, END, A, B, C, D, E, F) \
	typedef struct { A a; B b; C c; D d; E e; F f; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b, x->c, x->d, x->e, x->f); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b, C c, D d, E e, F f) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b, c, d, e, f); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; \
		END; \
	}
#define GLT_CALL7(fn, END, A, B, C, D, E, F, G) \
	typedef struct { A a; B b; C c; D d; E e; F f; G g; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b, x->c, x->d, x->e, x->f, x->g); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b, C c, D d, E e, F f, G g) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b, c, d, e, f, g); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; \
		END; \
	}
#define GLT_CALL10(fn, END, A, B, C, D, E, F, G, H, I, J) \
	typedef struct { A a; B b; C c; D d; E e; F f; G g; H h; I i; J j; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->h, x->i, x->j); \
	} \
	static void GLAPIENTRY glt_##fn(A a, B b, C c, D d, E e, F f, G g, H h, I i, J j) \
	{ \
		GLT_##fn##_Args *x; \
		if (!glt.active) { glt_real.fn(a, b, c, d, e, f, g, h, i, j); return; } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; \
		x->f = f; x->g = g; x->h = h; x->i = i; x->j = j; \
		END; \
	}

/* Calls that answer: the game waits for the answer */

#define GLT_RET0(R, fn) \
	typedef struct { R *r; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		*x->r = glt_real.fn(); \
	} \
	static R GLAPIENTRY glt_##fn(void) \
	{ \
		GLT_##fn##_Args *x; \
		R r; \
		if (!glt.active) { return glt_real.fn(); } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->r = &r; \
		GLThread_EndSync(); \
		return r; \
	}
#define GLT_RET1(R, fn, A) \
	typedef struct { A a; R *r; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		*x->r = glt_real.fn(x->a); \
	} \
	static R GLAPIENTRY glt_##fn(A a) \
	{ \
		GLT_##fn##_Args *x; \
		R r; \
		if (!glt.active) { return glt_real.fn(a); } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->r = &r; \
		GLThread_EndSync(); \
		return r; \
	}
#define GLT_RET2(R, fn, A, B) \
	typedef struct { A a; B b; R *r; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		*x->r = glt_real.fn(x->a, x->b); \
	} \
	static R GLAPIENTRY glt_##fn(A a, B b) \
	{ \
		GLT_##fn##_Args *x; \
		R r; \
		if (!glt.active) { return glt_real.fn(a, b); } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->r = &r; \
		GLThread_EndSync(); \
		return r; \
	}
#define GLT_RET4(R, fn, A, B, C, D) \
	typedef struct { A a; B b; C c; D d; R *r; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		*x->r = glt_real.fn(x->a, x->b, x->c, x->d); \
	} \
	static R GLAPIENTRY glt_##fn(A a, B b, C c, D d) \
	{ \
		GLT_##fn##_Args *x; \
		R r; \
		if (!glt.active) { return glt_real.fn(a, b, c, d); } \
		x = (GLT_##fn##_Args*) GLThread_Begin(GLT_##fn##_Run, sizeof(*x)); \
		x->a = a; x->b = b; x->c = c; x->d = d; x->r = &r; \
		GLThread_EndSync(); \
		return r; \
	}

#define GLT_ASYNC GLThread_End(0)
#define GLT_SYNC GLThread_EndSync()

/* State, binds and draws: the game goes on */
GLT_CALL1(glActiveTexture, GLT_ASYNC, GLenum)
GLT_CALL2(glBindBuffer, GLT_ASYNC, GLenum, GLuint)
GLT_CALL2(glBindTexture, GLT_ASYNC, GLenum, GLuint)
GLT_CALL4(glBlendColor, GLT_ASYNC, GLfloat, GLfloat, GLfloat, GLfloat)
GLT_CALL2(glBlendEquationSeparate, GLT_ASYNC, GLenum, GLenum)
GLT_CALL4(glBlendFuncSeparate, GLT_ASYNC, GLenum, GLenum, GLenum, GLenum)
GLT_CALL1(glClear, GLT_ASYNC, GLbitfield)
GLT_CALL4(glClearColor, GLT_ASYNC, GLclampf, GLclampf, GLclampf, GLclampf)
GLT_CALL1(glClearStencil, GLT_ASYNC, GLint)
GLT_CALL4(glColorMask, GLT_ASYNC, GLboolean, GLboolean, GLboolean, GLboolean)
GLT_CALL1(glDepthFunc, GLT_ASYNC, GLenum)
GLT_CALL1(glDepthMask, GLT_ASYNC, GLboolean)
GLT_CALL1(glDisable, GLT_ASYNC, GLenum)
GLT_CALL1(glDisableVertexAttribArray, GLT_ASYNC, GLint)
GLT_CALL3(glDrawArrays, GLT_ASYNC, GLenum, GLint, GLsizei)
/* Indices and attribute pointers are offsets into bound buffers: FNA3D draws from buffers only */
GLT_CALL6(glDrawRangeElements, GLT_ASYNC, GLenum, GLuint, GLuint, GLsizei, GLenum, const GLvoid*)
GLT_CALL1(glEnable, GLT_ASYNC, GLenum)
GLT_CALL1(glEnableVertexAttribArray, GLT_ASYNC, GLint)
GLT_CALL1(glFrontFace, GLT_ASYNC, GLenum)
GLT_CALL2(glPolygonOffset, GLT_ASYNC, GLfloat, GLfloat)
GLT_CALL4(glScissor, GLT_ASYNC, GLint, GLint, GLint, GLint)
GLT_CALL3(glStencilFunc, GLT_ASYNC, GLenum, GLint, GLint)
GLT_CALL4(glStencilFuncSeparate, GLT_ASYNC, GLenum, GLenum, GLenum, GLenum)
GLT_CALL3(glStencilOp, GLT_ASYNC, GLenum, GLenum, GLenum)
GLT_CALL4(glStencilOpSeparate, GLT_ASYNC, GLenum, GLenum, GLenum, GLenum)
GLT_CALL1(glStencilMask, GLT_ASYNC, GLint)
GLT_CALL3(glTexParameterf, GLT_ASYNC, GLenum, GLenum, GLfloat)
GLT_CALL3(glTexParameteri, GLT_ASYNC, GLenum, GLenum, GLint)
GLT_CALL6(glVertexAttribPointer, GLT_ASYNC, GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*)
GLT_CALL4(glViewport, GLT_ASYNC, GLint, GLint, GLsizei, GLsizei)
GLT_CALL1(glBindVertexArray, GLT_ASYNC, GLuint)
GLT_CALL6(glDrawElementsInstancedBaseVertex, GLT_ASYNC, GLenum, GLsizei, GLenum, const GLvoid*, GLsizei, GLint)
GLT_CALL7(glDrawRangeElementsBaseVertex, GLT_ASYNC, GLenum, GLuint, GLuint, GLsizei, GLenum, const GLvoid*, GLint)
GLT_CALL1(glClearDepth, GLT_ASYNC, GLdouble)
GLT_CALL2(glDepthRange, GLT_ASYNC, GLdouble, GLdouble)
GLT_CALL1(glClearDepthf, GLT_ASYNC, GLfloat)
GLT_CALL2(glDepthRangef, GLT_ASYNC, GLfloat, GLfloat)
GLT_CALL2(glBeginQuery, GLT_ASYNC, GLenum, GLuint)
GLT_CALL1(glEndQuery, GLT_ASYNC, GLenum)
GLT_CALL2(glPolygonMode, GLT_ASYNC, GLenum, GLenum)
GLT_CALL3(glTexEnvi, GLT_ASYNC, GLenum, GLenum, GLint)
GLT_CALL2(glBindFramebuffer, GLT_ASYNC, GLenum, GLuint)
GLT_CALL2(glBindRenderbuffer, GLT_ASYNC, GLenum, GLuint)
GLT_CALL4(glFramebufferRenderbuffer, GLT_ASYNC, GLenum, GLenum, GLenum, GLuint)
GLT_CALL5(glFramebufferTexture2D, GLT_ASYNC, GLenum, GLenum, GLenum, GLuint, GLint)
GLT_CALL1(glGenerateMipmap, GLT_ASYNC, GLenum)
GLT_CALL4(glRenderbufferStorage, GLT_ASYNC, GLenum, GLenum, GLsizei, GLsizei)
GLT_CALL10(glBlitFramebuffer, GLT_ASYNC, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
GLT_CALL5(glRenderbufferStorageMultisample, GLT_ASYNC, GLenum, GLsizei, GLenum, GLsizei, GLsizei)
GLT_CALL5(glDrawElementsInstanced, GLT_ASYNC, GLenum, GLsizei, GLenum, const GLvoid*, GLsizei)
GLT_CALL2(glVertexAttribDivisor, GLT_ASYNC, GLuint, GLuint)
GLT_CALL5(glColorMaski, GLT_ASYNC, GLuint, GLboolean, GLboolean, GLboolean, GLboolean)
GLT_CALL2(glSampleMaski, GLT_ASYNC, GLuint, GLuint)

/* MojoShader's GLSL calls */
GLT_CALL1(glDeleteShader, GLT_ASYNC, GLuint)
GLT_CALL1(glDeleteProgram, GLT_ASYNC, GLuint)
GLT_CALL2(glAttachShader, GLT_ASYNC, GLuint, GLuint)
GLT_CALL1(glCompileShader, GLT_ASYNC, GLuint)
GLT_CALL1(glLinkProgram, GLT_ASYNC, GLuint)
GLT_CALL2(glUniform1i, GLT_ASYNC, GLint, GLint)
GLT_CALL3(glUniform2f, GLT_ASYNC, GLint, GLfloat, GLfloat)
GLT_CALL2(glUniform1f, GLT_ASYNC, GLint, GLfloat)
GLT_CALL1(glUseProgram, GLT_ASYNC, GLuint)
GLT_CALL2(glVertexAttribDivisorARB, GLT_ASYNC, GLuint, GLuint)

/* Calls that write to the game's memory or answer: the game waits */
GLT_CALL2(glGetIntegerv, GLT_SYNC, GLenum, GLint*)
GLT_CALL7(glReadPixels, GLT_SYNC, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid*)
GLT_CALL2(glGenVertexArrays, GLT_SYNC, GLsizei, GLuint*)
GLT_CALL2(glGenQueries, GLT_SYNC, GLsizei, GLuint*)
GLT_CALL3(glGetQueryObjectuiv, GLT_SYNC, GLuint, GLenum, GLuint*)
GLT_CALL4(glGetBufferSubData, GLT_SYNC, GLenum, GLintptr, GLsizeiptr, GLvoid*)
GLT_CALL5(glGetTexImage, GLT_SYNC, GLenum, GLint, GLenum, GLenum, GLvoid*)
GLT_CALL2(glGenFramebuffers, GLT_SYNC, GLsizei, GLuint*)
GLT_CALL2(glGenRenderbuffers, GLT_SYNC, GLsizei, GLuint*)
GLT_CALL5(glGetInternalformativ, GLT_SYNC, GLenum, GLenum, GLenum, GLsizei, GLint*)
GLT_CALL2(glDebugMessageCallback, GLT_SYNC, DEBUGPROC, const GLvoid*)
GLT_CALL6(glDebugMessageControl, GLT_SYNC, GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean)
GLT_CALL2(glStringMarkerGREMEDY, GLT_SYNC, GLsizei, const GLchar*)
GLT_RET1(GLboolean, glUnmapBuffer, GLenum)
GLT_RET2(const GLubyte*, glGetStringi, GLenum, GLuint)
GLT_RET4(GLvoid*, glMapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield)
GLT_RET1(const GLubyte*, glGetString, GLenum)
GLT_RET0(GLenum, glGetError)
GLT_RET1(GLuint, glCreateShader, GLenum)
GLT_RET0(GLuint, glCreateProgram)
GLT_RET2(GLint, glGetAttribLocation, GLuint, const GLchar*)
GLT_RET2(GLint, glGetUniformLocation, GLuint, const GLchar*)
GLT_CALL4(glGetProgramInfoLog, GLT_SYNC, GLuint, GLsizei, GLsizei*, GLchar*)
GLT_CALL4(glGetShaderInfoLog, GLT_SYNC, GLuint, GLsizei, GLsizei*, GLchar*)
GLT_CALL3(glGetShaderiv, GLT_SYNC, GLuint, GLenum, GLint*)
GLT_CALL3(glGetProgramiv, GLT_SYNC, GLuint, GLenum, GLint*)
GLT_CALL4(glShaderSource, GLT_SYNC, GLuint, GLsizei, const GLchar* const*, const GLint*)


/* Names: textures and buffers come from names made ahead, GLT_POOL at a time, so making one
 * does not wait for the GL thread */

typedef void (GLAPIENTRY *GLThreadGen)(GLsizei a, GLuint *b);
typedef struct { GLThreadGen gen; GLsizei n; GLuint *out; } GLThreadGenArgs;

static void GLThread_GenRun(void *p)
{
	GLThreadGenArgs *x = (GLThreadGenArgs*) p;
	x->gen(x->n, x->out);
}

static void GLThread_Gen(GLThreadGen gen, GLuint *pool, int32_t *count, GLsizei n, GLuint *out)
{
	GLThreadGenArgs *x;
	GLsizei i;
	if (n > GLT_POOL)
	{
		x = (GLThreadGenArgs*) GLThread_Begin(GLThread_GenRun, sizeof(*x));
		x->gen = gen; x->n = n; x->out = out;
		GLThread_EndSync();
		return;
	}
	if (*count < n)
	{
		x = (GLThreadGenArgs*) GLThread_Begin(GLThread_GenRun, sizeof(*x));
		x->gen = gen; x->n = GLT_POOL - *count; x->out = pool + *count;
		GLThread_EndSync();
		*count = GLT_POOL;
	}
	for (i = 0; i < n; i += 1)
	{
		*count -= 1;
		out[i] = pool[*count];
	}
}

static void GLAPIENTRY glt_glGenTextures(GLsizei n, GLuint *out)
{
	if (!glt.active) { glt_real.glGenTextures(n, out); return; }
	GLThread_Gen(glt_real.glGenTextures, glt.texturePool, &glt.texturePoolCount, n, out);
}

static void GLAPIENTRY glt_glGenBuffers(GLint n, GLuint *out)
{
	if (!glt.active) { glt_real.glGenBuffers(n, out); return; }
	GLThread_Gen((GLThreadGen) glt_real.glGenBuffers, glt.bufferPool, &glt.bufferPoolCount, n, out);
}

/* Calls that read the game's memory: a copy goes with them */

#define GLT_ARRAY2(fn, N, T, per) \
	typedef struct { N n; GLThreadData d; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->n, (const T*) x->d.data); \
		GLThread_Release(&x->d); \
	} \
	static void GLAPIENTRY glt_##fn(N n, const T *v) \
	{ \
		GLT_##fn##_Args *x; \
		GLThreadData d; \
		if (!glt.active) { glt_real.fn(n, v); return; } \
		x = (GLT_##fn##_Args*) GLThread_BeginData(GLT_##fn##_Run, sizeof(*x), &d, v, (n > 0) ? (size_t) n * (per) : 0, 1); \
		x->n = n; x->d = d; \
		GLThread_EndData(&d); \
	}
#define GLT_ARRAY3(fn, A, N, T, per) \
	typedef struct { A a; N n; GLThreadData d; } GLT_##fn##_Args; \
	static void GLT_##fn##_Run(void *p) \
	{ \
		GLT_##fn##_Args *x = (GLT_##fn##_Args*) p; \
		glt_real.fn(x->a, x->n, (const T*) x->d.data); \
		GLThread_Release(&x->d); \
	} \
	static void GLAPIENTRY glt_##fn(A a, N n, const T *v) \
	{ \
		GLT_##fn##_Args *x; \
		GLThreadData d; \
		if (!glt.active) { glt_real.fn(a, n, v); return; } \
		x = (GLT_##fn##_Args*) GLThread_BeginData(GLT_##fn##_Run, sizeof(*x), &d, v, (n > 0) ? (size_t) n * (per) : 0, 1); \
		x->a = a; x->n = n; x->d = d; \
		GLThread_EndData(&d); \
	}

GLT_ARRAY2(glDeleteBuffers, GLsizei, GLuint, sizeof(GLuint))
GLT_ARRAY2(glDeleteTextures, GLsizei, GLuint, sizeof(GLuint))
GLT_ARRAY2(glDeleteVertexArrays, GLsizei, GLuint, sizeof(GLuint))
GLT_ARRAY2(glDeleteQueries, GLsizei, GLuint, sizeof(GLuint))
GLT_ARRAY2(glDeleteFramebuffers, GLsizei, GLuint, sizeof(GLuint))
GLT_ARRAY2(glDeleteRenderbuffers, GLsizei, GLuint, sizeof(GLuint))
GLT_ARRAY2(glDrawBuffers, GLsizei, GLenum, sizeof(GLenum))
GLT_ARRAY3(glInvalidateFramebuffer, GLenum, GLsizei, GLenum, sizeof(GLenum))
GLT_ARRAY3(glUniform1iv, GLint, GLsizei, GLint, sizeof(GLint))
GLT_ARRAY3(glUniform4fv, GLint, GLsizei, GLfloat, 4 * sizeof(GLfloat))
GLT_ARRAY3(glUniform4iv, GLint, GLsizei, GLint, 4 * sizeof(GLint))

typedef struct { GLenum target; GLsizeiptr size; GLenum usage; GLThreadData d; } GLT_glBufferData_Args;
static void GLT_glBufferData_Run(void *p)
{
	GLT_glBufferData_Args *x = (GLT_glBufferData_Args*) p;
	glt_real.glBufferData(x->target, x->size, x->d.data, x->usage);
	GLThread_Release(&x->d);
}
static void GLAPIENTRY glt_glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
	GLT_glBufferData_Args *x;
	GLThreadData d;
	if (!glt.active) { glt_real.glBufferData(target, size, data, usage); return; }
	x = (GLT_glBufferData_Args*) GLThread_BeginData(GLT_glBufferData_Run, sizeof(*x), &d, data, (size_t) size, 1);
	x->target = target; x->size = size; x->usage = usage; x->d = d;
	GLThread_EndData(&d);
}

typedef struct { GLenum target; GLintptr offset; GLsizeiptr size; GLThreadData d; } GLT_glBufferSubData_Args;
static void GLT_glBufferSubData_Run(void *p)
{
	GLT_glBufferSubData_Args *x = (GLT_glBufferSubData_Args*) p;
	glt_real.glBufferSubData(x->target, x->offset, x->size, x->d.data);
	GLThread_Release(&x->d);
}
static void GLAPIENTRY glt_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
	GLT_glBufferSubData_Args *x;
	GLThreadData d;
	if (!glt.active) { glt_real.glBufferSubData(target, offset, size, data); return; }
	x = (GLT_glBufferSubData_Args*) GLThread_BeginData(GLT_glBufferSubData_Run, sizeof(*x), &d, data, (size_t) size, 1);
	x->target = target; x->offset = offset; x->size = size; x->d = d;
	GLThread_EndData(&d);
}

/* Pixels: the unpack state is followed to size them; anything else is borrowed */

#define GLT_UNPACK_ROW_LENGTH		0x0CF2
#define GLT_UNPACK_SKIP_ROWS		0x0CF3
#define GLT_UNPACK_SKIP_PIXELS		0x0CF4
#define GLT_UNPACK_ALIGNMENT		0x0CF5
#define GLT_UNPACK_SKIP_IMAGES		0x806D
#define GLT_UNPACK_IMAGE_HEIGHT		0x806E

GLT_CALL2(glPixelStorei, GLT_ASYNC, GLenum, GLint)

static void GLAPIENTRY glt_glPixelStoreiTracked(GLenum pname, GLint value)
{
	uint32_t bit = 0;
	switch (pname)
	{
		case GLT_UNPACK_ALIGNMENT: glt.unpackAlignment = value; break;
		case GLT_UNPACK_ROW_LENGTH: bit = 1; break;
		case GLT_UNPACK_SKIP_ROWS: bit = 2; break;
		case GLT_UNPACK_SKIP_PIXELS: bit = 4; break;
		case GLT_UNPACK_SKIP_IMAGES: bit = 8; break;
		case GLT_UNPACK_IMAGE_HEIGHT: bit = 16; break;
		default: break;
	}
	if (bit)
	{
		glt.unpackOther = value ? (glt.unpackOther | bit) : (glt.unpackOther & ~bit);
	}
	glt_glPixelStorei(pname, value);
}

/* Bytes in a w x h x d image as the GL reads it from memory; 0 when we cannot tell */
static size_t GLThread_ImageSize(GLsizei w, GLsizei h, GLsizei depth, GLenum format, GLenum type)
{
	size_t channels, bytes, pixel, row, align;
	if (w <= 0 || h <= 0 || depth <= 0 || glt.unpackOther != 0)
	{
		return 0;
	}
	switch (format)
	{
		case 0x1903: /* RED */
		case 0x1906: /* ALPHA */
		case 0x1909: /* LUMINANCE */
		case 0x1902: /* DEPTH_COMPONENT */
		case 0x8D94: /* RED_INTEGER */
			channels = 1; break;
		case 0x8227: /* RG */
		case 0x190A: /* LUMINANCE_ALPHA */
		case 0x84F9: /* DEPTH_STENCIL */
		case 0x8228: /* RG_INTEGER */
			channels = 2; break;
		case 0x1907: /* RGB */
		case 0x80E0: /* BGR */
			channels = 3; break;
		case 0x1908: /* RGBA */
		case 0x80E1: /* BGRA */
			channels = 4; break;
		default:
			return 0;
	}
	switch (type)
	{
		case 0x1400: /* BYTE */
		case 0x1401: /* UNSIGNED_BYTE */
			bytes = 1; pixel = channels; break;
		case 0x1402: /* SHORT */
		case 0x1403: /* UNSIGNED_SHORT */
		case 0x140B: /* HALF_FLOAT */
		case 0x8D61: /* HALF_FLOAT_OES */
			bytes = 2; pixel = 2 * channels; break;
		case 0x1404: /* INT */
		case 0x1405: /* UNSIGNED_INT */
		case 0x1406: /* FLOAT */
			bytes = 4; pixel = 4 * channels; break;
		case 0x8363: /* UNSIGNED_SHORT_5_6_5 */
		case 0x8364: /* UNSIGNED_SHORT_5_6_5_REV */
		case 0x8033: /* UNSIGNED_SHORT_4_4_4_4 */
		case 0x8365: /* UNSIGNED_SHORT_4_4_4_4_REV */
		case 0x8034: /* UNSIGNED_SHORT_5_5_5_1 */
		case 0x8366: /* UNSIGNED_SHORT_1_5_5_5_REV */
			bytes = 2; pixel = 2; break;
		case 0x8035: /* UNSIGNED_INT_8_8_8_8 */
		case 0x8367: /* UNSIGNED_INT_8_8_8_8_REV */
		case 0x8036: /* UNSIGNED_INT_10_10_10_2 */
		case 0x8368: /* UNSIGNED_INT_2_10_10_10_REV */
		case 0x84FA: /* UNSIGNED_INT_24_8 */
		case 0x8C3B: /* UNSIGNED_INT_10F_11F_11F_REV */
		case 0x8C3E: /* UNSIGNED_INT_5_9_9_9_REV */
			bytes = 4; pixel = 4; break;
		default:
			return 0;
	}
	row = (size_t) w * pixel;
	align = (glt.unpackAlignment > 0) ? (size_t) glt.unpackAlignment : 4;
	if (bytes < align)
	{
		row = (row + align - 1) / align * align;
	}
	return row * ((size_t) h * (size_t) depth - 1) + (size_t) w * pixel;
}

typedef struct { GLenum a; GLint b; GLint c; GLsizei d; GLsizei e; GLint f; GLenum g; GLenum h; GLThreadData data; } GLT_glTexImage2D_Args;
static void GLT_glTexImage2D_Run(void *p)
{
	GLT_glTexImage2D_Args *x = (GLT_glTexImage2D_Args*) p;
	glt_real.glTexImage2D(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->h, x->data.data);
	GLThread_Release(&x->data);
}
static void GLAPIENTRY glt_glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const GLvoid *i)
{
	GLT_glTexImage2D_Args *x;
	GLThreadData data;
	size_t size;
	if (!glt.active) { glt_real.glTexImage2D(a, b, c, d, e, f, g, h, i); return; }
	size = GLThread_ImageSize(d, e, 1, g, h);
	x = (GLT_glTexImage2D_Args*) GLThread_BeginData(GLT_glTexImage2D_Run, sizeof(*x), &data, i, size, size > 0);
	x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; x->h = h; x->data = data;
	GLThread_EndData(&data);
}

typedef struct { GLenum a; GLint b; GLint c; GLint d; GLsizei e; GLsizei f; GLenum g; GLenum h; GLThreadData data; } GLT_glTexSubImage2D_Args;
static void GLT_glTexSubImage2D_Run(void *p)
{
	GLT_glTexSubImage2D_Args *x = (GLT_glTexSubImage2D_Args*) p;
	glt_real.glTexSubImage2D(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->h, x->data.data);
	GLThread_Release(&x->data);
}
static void GLAPIENTRY glt_glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const GLvoid *i)
{
	GLT_glTexSubImage2D_Args *x;
	GLThreadData data;
	size_t size;
	if (!glt.active) { glt_real.glTexSubImage2D(a, b, c, d, e, f, g, h, i); return; }
	size = GLThread_ImageSize(e, f, 1, g, h);
	x = (GLT_glTexSubImage2D_Args*) GLThread_BeginData(GLT_glTexSubImage2D_Run, sizeof(*x), &data, i, size, size > 0);
	x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; x->h = h; x->data = data;
	GLThread_EndData(&data);
}

typedef struct { GLenum a; GLint b; GLint c; GLsizei d; GLsizei e; GLsizei f; GLint g; GLenum h; GLenum i; GLThreadData data; } GLT_glTexImage3D_Args;
static void GLT_glTexImage3D_Run(void *p)
{
	GLT_glTexImage3D_Args *x = (GLT_glTexImage3D_Args*) p;
	glt_real.glTexImage3D(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->h, x->i, x->data.data);
	GLThread_Release(&x->data);
}
static void GLAPIENTRY glt_glTexImage3D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLsizei f, GLint g, GLenum h, GLenum i, const GLvoid *j)
{
	GLT_glTexImage3D_Args *x;
	GLThreadData data;
	size_t size;
	if (!glt.active) { glt_real.glTexImage3D(a, b, c, d, e, f, g, h, i, j); return; }
	size = GLThread_ImageSize(d, e, f, h, i);
	x = (GLT_glTexImage3D_Args*) GLThread_BeginData(GLT_glTexImage3D_Run, sizeof(*x), &data, j, size, size > 0);
	x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; x->h = h; x->i = i; x->data = data;
	GLThread_EndData(&data);
}

typedef struct { GLenum a; GLint b; GLint c; GLint d; GLint e; GLsizei f; GLsizei g; GLsizei h; GLenum i; GLenum j; GLThreadData data; } GLT_glTexSubImage3D_Args;
static void GLT_glTexSubImage3D_Run(void *p)
{
	GLT_glTexSubImage3D_Args *x = (GLT_glTexSubImage3D_Args*) p;
	glt_real.glTexSubImage3D(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->h, x->i, x->j, x->data.data);
	GLThread_Release(&x->data);
}
static void GLAPIENTRY glt_glTexSubImage3D(GLenum a, GLint b, GLint c, GLint d, GLint e, GLsizei f, GLsizei g, GLsizei h, GLenum i, GLenum j, const GLvoid *k)
{
	GLT_glTexSubImage3D_Args *x;
	GLThreadData data;
	size_t size;
	if (!glt.active) { glt_real.glTexSubImage3D(a, b, c, d, e, f, g, h, i, j, k); return; }
	size = GLThread_ImageSize(f, g, h, i, j);
	x = (GLT_glTexSubImage3D_Args*) GLThread_BeginData(GLT_glTexSubImage3D_Run, sizeof(*x), &data, k, size, size > 0);
	x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; x->h = h; x->i = i; x->j = j; x->data = data;
	GLThread_EndData(&data);
}

typedef struct { GLenum a; GLint b; GLenum c; GLsizei d; GLsizei e; GLint f; GLsizei g; GLThreadData data; } GLT_glCompressedTexImage2D_Args;
static void GLT_glCompressedTexImage2D_Run(void *p)
{
	GLT_glCompressedTexImage2D_Args *x = (GLT_glCompressedTexImage2D_Args*) p;
	glt_real.glCompressedTexImage2D(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->data.data);
	GLThread_Release(&x->data);
}
static void GLAPIENTRY glt_glCompressedTexImage2D(GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const GLvoid *h)
{
	GLT_glCompressedTexImage2D_Args *x;
	GLThreadData data;
	if (!glt.active) { glt_real.glCompressedTexImage2D(a, b, c, d, e, f, g, h); return; }
	x = (GLT_glCompressedTexImage2D_Args*) GLThread_BeginData(GLT_glCompressedTexImage2D_Run, sizeof(*x), &data, h, (g > 0) ? (size_t) g : 0, g > 0);
	x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; x->data = data;
	GLThread_EndData(&data);
}

typedef struct { GLenum a; GLint b; GLint c; GLint d; GLsizei e; GLsizei f; GLenum g; GLsizei h; GLThreadData data; } GLT_glCompressedTexSubImage2D_Args;
static void GLT_glCompressedTexSubImage2D_Run(void *p)
{
	GLT_glCompressedTexSubImage2D_Args *x = (GLT_glCompressedTexSubImage2D_Args*) p;
	glt_real.glCompressedTexSubImage2D(x->a, x->b, x->c, x->d, x->e, x->f, x->g, x->h, x->data.data);
	GLThread_Release(&x->data);
}
static void GLAPIENTRY glt_glCompressedTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLsizei h, const GLvoid *i)
{
	GLT_glCompressedTexSubImage2D_Args *x;
	GLThreadData data;
	if (!glt.active) { glt_real.glCompressedTexSubImage2D(a, b, c, d, e, f, g, h, i); return; }
	x = (GLT_glCompressedTexSubImage2D_Args*) GLThread_BeginData(GLT_glCompressedTexSubImage2D_Run, sizeof(*x), &data, i, (h > 0) ? (size_t) h : 0, h > 0);
	x->a = a; x->b = b; x->c = c; x->d = d; x->e = e; x->f = f; x->g = g; x->h = h; x->data = data;
	GLThread_EndData(&data);
}

/* The window: swaps and the swap interval */

#ifdef USE_SDL3
typedef bool GLThreadSwapResult;
#else
typedef int GLThreadSwapResult;
#endif

typedef struct { SDL_Window *window; } GLThreadSwapArgs;
static void GLThread_SwapRun(void *p)
{
	GLThreadSwapArgs *x = (GLThreadSwapArgs*) p;
	SDL_GL_SwapWindow(x->window);
	GLT_ADD(&glt.swapsDone, 1);
}

static void GLThread_LogStats(void);

static void GLThread_SwapWindow(SDL_Window *window)
{
	GLThreadSwapArgs *x;
	if (!glt.active)
	{
		SDL_GL_SwapWindow(window);
		return;
	}
	x = (GLThreadSwapArgs*) GLThread_Begin(GLThread_SwapRun, sizeof(*x));
	x->window = window;
	GLThread_End(1);
	glt.swapsQueued += 1;
	GLT_WAIT(
		GLT_LOAD(&glt.swapsDone) + GLT_FRAMES >= glt.swapsQueued,
		glt.statSwapWaitTicks
	)
	if (glt.stats && ++glt.statFrames == 600)
	{
		GLThread_LogStats();
	}
}

typedef struct { int interval; GLThreadSwapResult *result; } GLThreadIntervalArgs;
static void GLThread_IntervalRun(void *p)
{
	GLThreadIntervalArgs *x = (GLThreadIntervalArgs*) p;
	*x->result = SDL_GL_SetSwapInterval(x->interval);
}

static GLThreadSwapResult GLThread_SetSwapInterval(int interval)
{
	GLThreadIntervalArgs *x;
	GLThreadSwapResult result;
	if (!glt.active)
	{
		return SDL_GL_SetSwapInterval(interval);
	}
	x = (GLThreadIntervalArgs*) GLThread_Begin(GLThread_IntervalRun, sizeof(*x));
	x->interval = interval;
	x->result = &result;
	GLThread_EndSync();
	return result;
}

/* The GL thread */

static void GLThread_QuitRun(void *p)
{
	glt.quit = 1;
}

static int GLThread_Main(void *data)
{
	uint64_t r = 0, w;
	int32_t spin;
	GLThreadCmd *cmd;
	uint8_t ok;

#ifdef USE_SDL3
	ok = SDL_GL_MakeCurrent(glt.window, glt.context);
#else
	ok = SDL_GL_MakeCurrent(glt.window, glt.context) == 0;
#endif
	GLT_STORE(&glt.started, ok ? 1 : -1);
	SDL_SignalSemaphore(glt.wakeGame);
	if (!ok)
	{
		return 0;
	}

	while (!glt.quit)
	{
		w = GLT_LOAD(&glt.w);
		for (spin = 0; r == w && spin < GLT_SPIN; spin += 1)
		{
			GLT_RELAX();
			w = GLT_LOAD(&glt.w);
		}
		if (r == w)
		{
			GLT_STORE(&glt.glSleeping, 1);
			if (r == GLT_LOAD(&glt.w))
			{
				SDL_WaitSemaphore(glt.wakeGL);
			}
			GLT_STORE(&glt.glSleeping, 0);
			continue;
		}
		while (r != w && !glt.quit)
		{
			cmd = (GLThreadCmd*) (glt.ring + (r & GLT_RING_MASK));
			if (cmd->run != NULL)
			{
				cmd->run(((uint8_t*) cmd) + GLT_HDR);
			}
			r += cmd->size;
			GLT_STORE(&glt.r, r);
			if (GLT_LOAD(&glt.gameWaiting) && GLT_XCHG(&glt.gameWaiting, 0))
			{
				SDL_SignalSemaphore(glt.wakeGame);
			}
		}
	}

	/* The context goes back to the game's thread */
	SDL_GL_MakeCurrent(glt.window, NULL);
	return 0;
}

static void GLThread_LogStats(void)
{
	uint64_t now = SDL_GetPerformanceCounter();
	double f = (double) glt.statFrames;
	double ms = 1000.0 / (double) SDL_GetPerformanceFrequency();
	FNA3D_LogInfo(
		"GL thread: %.1f fps; a frame: %.0f kB of calls, %.1f syncs, %.1f waits (%.2f ms, swap %.2f ms), %.1f wakes, %.2f heap copies",
		f / ((double) (now - glt.statStart) * ms / 1000.0),
		(double) glt.statBytes / f / 1024.0,
		(double) glt.statSyncs / f,
		(double) glt.statWaits / f,
		(double) glt.statWaitTicks * ms / f,
		(double) glt.statSwapWaitTicks * ms / f,
		(double) glt.statWakes / f,
		(double) glt.statHeap / f
	);
	glt.statFrames = 0;
	glt.statSyncs = glt.statWaits = glt.statWakes = glt.statHeap = glt.statBytes = 0;
	glt.statWaitTicks = glt.statSwapWaitTicks = 0;
	glt.statStart = now;
}

/* MojoShader's entry points: wrapped when the thread is wanted, the driver's own kept */

#define GLT_ENTRY(fn) { #fn, (void**) &glt_real.fn, (void*) glt_##fn }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static const struct { const char *name; void **real; void *proxy; } GLThread_Entries[] =
{
	GLT_ENTRY(glGetString),
	GLT_ENTRY(glGetError),
	GLT_ENTRY(glGetIntegerv),
	GLT_ENTRY(glEnable),
	GLT_ENTRY(glDisable),
	GLT_ENTRY(glGetStringi),
	GLT_ENTRY(glDeleteShader),
	GLT_ENTRY(glDeleteProgram),
	GLT_ENTRY(glAttachShader),
	GLT_ENTRY(glCompileShader),
	GLT_ENTRY(glCreateShader),
	GLT_ENTRY(glCreateProgram),
	GLT_ENTRY(glDisableVertexAttribArray),
	GLT_ENTRY(glEnableVertexAttribArray),
	GLT_ENTRY(glGetAttribLocation),
	GLT_ENTRY(glGetProgramInfoLog),
	GLT_ENTRY(glGetShaderInfoLog),
	GLT_ENTRY(glGetShaderiv),
	GLT_ENTRY(glGetProgramiv),
	GLT_ENTRY(glGetUniformLocation),
	GLT_ENTRY(glLinkProgram),
	GLT_ENTRY(glShaderSource),
	GLT_ENTRY(glUniform1i),
	GLT_ENTRY(glUniform1iv),
	GLT_ENTRY(glUniform2f),
	GLT_ENTRY(glUniform1f),
	GLT_ENTRY(glUniform4fv),
	GLT_ENTRY(glUniform4iv),
	GLT_ENTRY(glUseProgram),
	GLT_ENTRY(glVertexAttribPointer),
	GLT_ENTRY(glVertexAttribDivisorARB),
};
#pragma GCC diagnostic pop
#undef GLT_ENTRY

static void* GLThread_Proxy(const char *name, void *real)
{
	size_t i;
	if (!glt.wanted || real == NULL)
	{
		return real;
	}
	for (i = 0; i < SDL_arraysize(GLThread_Entries); i += 1)
	{
		if (SDL_strcmp(name, GLThread_Entries[i].name) == 0)
		{
			if (*GLThread_Entries[i].real == NULL)
			{
				*GLThread_Entries[i].real = real;
			}
			return GLThread_Entries[i].proxy;
		}
	}
	/* Not one of the GLSL profiles' calls: the thread will not start for this profile */
	return real;
}

/* Before the context is made */
static void GLThread_Init(void)
{
	SDL_memset(&glt, '\0', sizeof(glt));
	SDL_memset(&glt_real, '\0', sizeof(glt_real));
	glt.wanted = GLT_SUPPORTED && SDL_GetHintBoolean("FNA3D_OPENGL_THREAD", 0);
	glt.unpackAlignment = 4;
}

/* At the end of OPENGL_CreateDevice: the driver's entry points wrapped, the context handed over */
static void GLThread_Start(
	OpenGLRenderer *renderer,
	SDL_Window *window,
	uint8_t debugMode
) {
	if (!glt.wanted)
	{
		return;
	}
	if (debugMode || SDL_strncmp(renderer->shaderProfile, "glsl", 4) != 0)
	{
		FNA3D_LogWarn(
			"GL thread: not with %s",
			debugMode ? "a debug context" : renderer->shaderProfile
		);
		return;
	}

	/* The driver's calls go through the ring from now on (each one only where it exists) */
	glt_real.glGetString = renderer->glGetString;
	renderer->glGetString = glt_glGetString;
	#define GL_EXT(ext)
	#define GL_PROC(ext, ret, func, parms) \
		if (renderer->func != NULL) \
		{ \
			glt_real.func = renderer->func; \
			renderer->func = glt_##func; \
		}
	#define GL_PROC_EXT(ext, fallback, ret, func, parms) \
		GL_PROC(ext, ret, func, parms)
	#include "FNA3D_Driver_OpenGL_glfuncs.h"
	if (renderer->glPixelStorei != NULL)
	{
		renderer->glPixelStorei = glt_glPixelStoreiTracked;
	}

	glt.ring = (uint8_t*) SDL_malloc(GLT_RING_SIZE);
	glt.wakeGL = SDL_CreateSemaphore(0);
	glt.wakeGame = SDL_CreateSemaphore(0);
	if (glt.ring == NULL || glt.wakeGL == NULL || glt.wakeGame == NULL)
	{
		FNA3D_LogWarn("GL thread: out of memory");
		goto fail;
	}

	glt.window = window;
	glt.context = renderer->context;
	SDL_GL_MakeCurrent(window, NULL);
	glt.thread = SDL_CreateThread(GLThread_Main, "FNA3D GL", NULL);
	if (glt.thread == NULL)
	{
		FNA3D_LogWarn("GL thread: %s", SDL_GetError());
		SDL_GL_MakeCurrent(window, renderer->context);
		goto fail;
	}
	while (GLT_LOAD(&glt.started) == 0)
	{
		SDL_WaitSemaphore(glt.wakeGame);
	}
	if (GLT_LOAD(&glt.started) < 0)
	{
		FNA3D_LogWarn("GL thread: the context would not move: %s", SDL_GetError());
		SDL_WaitThread(glt.thread, NULL);
		glt.thread = NULL;
		SDL_GL_MakeCurrent(window, renderer->context);
		goto fail;
	}

	glt.stats = SDL_GetHintBoolean("FNA3D_OPENGL_THREAD_STATS", 0);
	glt.statStart = SDL_GetPerformanceCounter();
	glt.active = 1;
	FNA3D_LogInfo("GL thread: the OpenGL calls run on a thread of their own");
	return;

fail:
	/* The wrappers stay, calling the driver directly */
	if (glt.wakeGL != NULL) SDL_DestroySemaphore(glt.wakeGL);
	if (glt.wakeGame != NULL) SDL_DestroySemaphore(glt.wakeGame);
	SDL_free(glt.ring);
	glt.ring = NULL;
	glt.wakeGL = glt.wakeGame = NULL;
}

/* First thing in OPENGL_DestroyDevice: every call made, the context back on this thread */
static void GLThread_Stop(void)
{
	if (!glt.active)
	{
		return;
	}
	GLThread_Begin(GLThread_QuitRun, 0);
	GLThread_End(1);
	SDL_WaitThread(glt.thread, NULL);
	glt.thread = NULL;
	glt.active = 0;
	SDL_GL_MakeCurrent(glt.window, glt.context);

	/* The names made ahead and never used */
	if (glt.texturePoolCount > 0)
	{
		glt_real.glDeleteTextures(glt.texturePoolCount, glt.texturePool);
	}
	if (glt.bufferPoolCount > 0)
	{
		glt_real.glDeleteBuffers(glt.bufferPoolCount, glt.bufferPool);
	}
	glt.texturePoolCount = glt.bufferPoolCount = 0;

	SDL_DestroySemaphore(glt.wakeGL);
	SDL_DestroySemaphore(glt.wakeGame);
	SDL_free(glt.ring);
	glt.ring = NULL;
	glt.wakeGL = glt.wakeGame = NULL;
}
