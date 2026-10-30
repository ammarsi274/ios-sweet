/*
 * pthread, semaphores, TLS keys and errno for the Nokia's driver, on the
 * host futex (Linux futex on the bench, __ulock on iOS).
 *
 * Layouts (bionic arm64 sizes; only the first words are used):
 *   pthread_mutex_t 40 bytes: u32 state (bits 0-1 lock: 0 free, 1 locked,
 *     2 locked with waiters; bits 14-15 type 0 normal, 1 recursive,
 *     2 errorcheck, the encoding of bionic's static initialisers), u32 owner
 *     tid, u32 recursion count
 *   pthread_mutexattr_t 8 bytes: bits 0-3 type, bit 4 process-shared
 *   pthread_cond_t 48 bytes: u32 sequence, u32 waiters
 *   pthread_rwlock_t 56 bytes: u32 state (readers, or ~0 for a writer), u32 waiters
 *   sem_t 16 bytes: u32 value, u32 waiters
 *   pthread_key_t int, pthread_t long (the host thread handle)
 * Static initialisers of bionic are all zero except the mutex type bits.
 */
#include "bionic.h"

static uint32_t ld(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int cas(volatile uint32_t *p, uint32_t old, uint32_t new)
{
	return __atomic_compare_exchange_n(p, &old, new, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* ---- per-thread state ---- */
struct bthread *bthread_self(void)
{
	struct bthread *t = host_tls_get();
	if (!t) {
		t = host_alloc(sizeof(*t));
		host_tls_set(t);
	}
	return t;
}

int *b___errno(void)
{
	return &bthread_self()->err;
}

long b_ret(long r)
{
	if (r < 0 && r > -4096) {
		B_ERR((int)-r);
		return -1;
	}
	return r;
}

/* absolute time on `clock` -> relative ns, <0 if passed */
static int64_t until(int clock, const struct host_ts *abs)
{
	struct host_ts now;
	host_clock(clock, &now);
	return (abs->sec - now.sec) * 1000000000LL + (abs->nsec - now.nsec);
}

/* ---- mutex ---- */
struct bmutex {
	volatile uint32_t state;
	volatile uint32_t owner;
	uint32_t count;
};
#define MTYPE(s) (((s) >> 14) & 3)

static void lock_slow(struct bmutex *m)
{
	for (;;) {
		uint32_t old = ld(&m->state);
		if (cas(&m->state, old, (old & ~3u) | 2)) {
			if (!(old & 3))
				return;
			host_futex_wait(&m->state, (old & ~3u) | 2, -1);
		}
	}
}

int b_pthread_mutex_lock(struct bmutex *m)
{
	uint32_t s = ld(&m->state), hi = s & ~3u;
	if (MTYPE(s)) {
		uint32_t tid = (uint32_t)host_gettid();
		if (m->owner == tid) {
			if (MTYPE(s) == 2)
				return LE_DEADLK;
			m->count++;
			return 0;
		}
		if (!cas(&m->state, hi, hi | 1))
			lock_slow(m);
		m->owner = tid;
		m->count = 0;
		return 0;
	}
	if (!cas(&m->state, hi, hi | 1))
		lock_slow(m);
	return 0;
}

int b_pthread_mutex_trylock(struct bmutex *m)
{
	uint32_t s = ld(&m->state), hi = s & ~3u;
	if (MTYPE(s)) {
		uint32_t tid = (uint32_t)host_gettid();
		if (m->owner == tid) {
			if (MTYPE(s) == 2)
				return LE_BUSY;
			m->count++;
			return 0;
		}
		if (!cas(&m->state, hi, hi | 1))
			return LE_BUSY;
		m->owner = tid;
		m->count = 0;
		return 0;
	}
	return cas(&m->state, hi, hi | 1) ? 0 : LE_BUSY;
}

int b_pthread_mutex_unlock(struct bmutex *m)
{
	uint32_t s = ld(&m->state);
	if (MTYPE(s)) {
		if (m->owner != (uint32_t)host_gettid())
			return LE_PERM;
		if (m->count) {
			m->count--;
			return 0;
		}
		m->owner = 0;
	}
	uint32_t old;
	do
		old = ld(&m->state);
	while (!cas(&m->state, old, old & ~3u));
	if ((old & 3) == 2)
		host_futex_wake(&m->state, 1);
	return 0;
}

int b_pthread_mutexattr_init(uint64_t *a)
{
	*a = 0;
	return 0;
}

int b_pthread_mutexattr_destroy(uint64_t *a)
{
	return 0;
}

int b_pthread_mutexattr_settype(uint64_t *a, int type)
{
	if (type < 0 || type > 2)
		return LE_INVAL;
	*a = (*a & ~0xfULL) | (uint64_t)type;
	return 0;
}

int b_pthread_mutexattr_setpshared(uint64_t *a, int shared)
{
	*a = shared ? *a | 0x10 : *a & ~0x10ULL;
	return 0;
}

int b_pthread_mutex_init(struct bmutex *m, const uint64_t *a)
{
	uint64_t at = a ? *a : 0;
	m->state = (uint32_t)(at & 3) << 14 | (at & 0x10 ? 1u << 13 : 0);
	m->owner = 0;
	m->count = 0;
	return 0;
}

int b_pthread_mutex_destroy(struct bmutex *m)
{
	return (ld(&m->state) & 3) ? LE_BUSY : 0;
}

/* ---- condition variable (CLOCK_REALTIME, bionic's default) ---- */
struct bcond {
	volatile uint32_t seq;
	volatile uint32_t waiters;
};

int b_pthread_cond_init(struct bcond *c, const void *attr)
{
	c->seq = 0;
	c->waiters = 0;
	return 0;
}

int b_pthread_cond_destroy(struct bcond *c)
{
	return 0;
}

static int cond_wait(struct bcond *c, struct bmutex *m, const struct host_ts *abs)
{
	uint32_t seq = ld(&c->seq);
	__atomic_add_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
	b_pthread_mutex_unlock(m);
	int r = 0;
	if (abs) {
		int64_t rel = until(0, abs);
		r = rel <= 0 ? -LE_TIMEDOUT : host_futex_wait(&c->seq, seq, rel);
	} else
		host_futex_wait(&c->seq, seq, -1);
	__atomic_sub_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
	/* take the mutex as contended, so the unlock after us wakes the next */
	uint32_t s = ld(&m->state);
	if (MTYPE(s))
		b_pthread_mutex_lock(m);
	else
		lock_slow(m);
	return r == -LE_TIMEDOUT ? LE_TIMEDOUT : 0;
}

int b_pthread_cond_wait(struct bcond *c, struct bmutex *m)
{
	return cond_wait(c, m, 0);
}

int b_pthread_cond_timedwait(struct bcond *c, struct bmutex *m, const struct host_ts *abs)
{
	return cond_wait(c, m, abs);
}

int b_pthread_cond_signal(struct bcond *c)
{
	__atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
	if (ld(&c->waiters))
		host_futex_wake(&c->seq, 1);
	return 0;
}

int b_pthread_cond_broadcast(struct bcond *c)
{
	__atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
	if (ld(&c->waiters))
		host_futex_wake(&c->seq, 0x7fffffff);
	return 0;
}

/* ---- rwlock ---- */
struct brw {
	volatile uint32_t state;
	volatile uint32_t waiters;
};
#define WRITER 0xffffffffu

int b_pthread_rwlock_init(struct brw *l, const void *attr)
{
	l->state = 0;
	l->waiters = 0;
	return 0;
}

int b_pthread_rwlock_destroy(struct brw *l)
{
	return ld(&l->state) ? LE_BUSY : 0;
}

int b_pthread_rwlock_rdlock(struct brw *l)
{
	for (;;) {
		uint32_t s = ld(&l->state);
		if (s != WRITER && s + 1 != WRITER && cas(&l->state, s, s + 1))
			return 0;
		if (s == WRITER) {
			__atomic_add_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
			host_futex_wait(&l->state, s, -1);
			__atomic_sub_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
		}
	}
}

int b_pthread_rwlock_wrlock(struct brw *l)
{
	for (;;) {
		if (cas(&l->state, 0, WRITER))
			return 0;
		uint32_t s = ld(&l->state);
		if (s) {
			__atomic_add_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
			host_futex_wait(&l->state, s, -1);
			__atomic_sub_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
		}
	}
}

int b_pthread_rwlock_unlock(struct brw *l)
{
	uint32_t s, n;
	do {
		s = ld(&l->state);
		if (!s)
			return LE_PERM;
		n = s == WRITER ? 0 : s - 1;
	} while (!cas(&l->state, s, n));
	if (!n && ld(&l->waiters))
		host_futex_wake(&l->state, 0x7fffffff);
	return 0;
}

/* ---- semaphores (POSIX: -1 and errno) ---- */
struct bsem {
	volatile uint32_t value;
	volatile uint32_t waiters;
};

int b_sem_init(struct bsem *s, int shared, unsigned value)
{
	s->value = value;
	s->waiters = 0;
	return 0;
}

int b_sem_destroy(struct bsem *s)
{
	return 0;
}

int b_sem_post(struct bsem *s)
{
	__atomic_add_fetch(&s->value, 1, __ATOMIC_ACQ_REL);
	if (ld(&s->waiters))
		host_futex_wake(&s->value, 1);
	return 0;
}

static int sem_wait_until(struct bsem *s, int clock, const struct host_ts *abs)
{
	for (;;) {
		uint32_t v = ld(&s->value);
		if (v && cas(&s->value, v, v - 1))
			return 0;
		if (v)
			continue;
		int64_t rel = -1;
		if (abs && (rel = until(clock, abs)) <= 0) {
			B_ERR(LE_TIMEDOUT);
			return -1;
		}
		__atomic_add_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
		host_futex_wait(&s->value, 0, rel);
		__atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
	}
}

int b_sem_wait(struct bsem *s)
{
	return sem_wait_until(s, 0, 0);
}

int b_sem_timedwait(struct bsem *s, const struct host_ts *abs)
{
	return sem_wait_until(s, 0, abs);
}

int b_sem_timedwait_monotonic_np(struct bsem *s, const struct host_ts *abs)
{
	return sem_wait_until(s, 1, abs);
}

/* ---- TLS keys ---- */
static volatile uint32_t key_used[BKEYS];
static void (*key_dtor[BKEYS])(void *);

int b_pthread_key_create(int *k, void (*dtor)(void *))
{
	for (int i = 0; i < BKEYS; i++)
		if (cas(&key_used[i], 0, 1)) {
			key_dtor[i] = dtor;
			*k = i;
			return 0;
		}
	return LE_AGAIN;
}

int b_pthread_key_delete(int k)
{
	if (k < 0 || k >= BKEYS)
		return LE_INVAL;
	key_dtor[k] = 0;
	key_used[k] = 0;
	return 0;
}

void *b_pthread_getspecific(int k)
{
	return k >= 0 && k < BKEYS ? bthread_self()->keys[k] : 0;
}

int b_pthread_setspecific(int k, const void *v)
{
	if (k < 0 || k >= BKEYS || !key_used[k])
		return LE_INVAL;
	bthread_self()->keys[k] = (void *)v;
	return 0;
}

static void run_key_dtors(struct bthread *t)
{
	for (int round = 0; round < 4; round++) {
		int any = 0;
		for (int i = 0; i < BKEYS; i++) {
			void *v = t->keys[i];
			if (v && key_used[i] && key_dtor[i]) {
				t->keys[i] = 0;
				key_dtor[i](v);
				any = 1;
			}
		}
		if (!any)
			break;
	}
}

/* ---- threads ---- */
static void *thread_start(void *p)
{
	struct bthread *t = p;
	host_tls_set(t);
	void *r = t->fn(t->arg);
	run_key_dtors(t);
	host_tls_set(0);
	host_free(t);
	return r;
}

/* bionic pthread_attr_t: u32 flags, void *stack_base, size_t stack_size, ... */
int b_pthread_create(uint64_t *thr, const void *attr, void *(*fn)(void *), void *arg)
{
	size_t stack = 1 << 20;
	if (attr) {
		size_t want = *(const size_t *)((const char *)attr + 16);
		if (want >= 16384 && want <= (64u << 20))
			stack = want;
	}
	struct bthread *t = host_alloc(sizeof(*t));
	t->fn = fn;
	t->arg = arg;
	int r = host_thread_create(thread_start, t, stack, thr);
	if (r) {
		host_free(t);
		return -r;
	}
	return 0;
}

int b_pthread_join(uint64_t t, void **ret)
{
	return -host_thread_join(t, ret);
}

int b_pthread_detach(uint64_t t)
{
	return -host_thread_detach(t);
}

uint64_t b_pthread_self(void)
{
	return host_thread_self();
}

int b_pthread_equal(uint64_t a, uint64_t b)
{
	return a == b;
}

void b_pthread_exit(void *ret)
{
	struct bthread *t = host_tls_get();
	if (t) {
		run_key_dtors(t);
		host_tls_set(0);
		host_free(t);
	}
	host_thread_exit(ret);
}

int b_gettid(void)
{
	return host_gettid();
}

int b_getpid(void)
{
	return host_getpid();
}

/* the driver pins its threads to cores; neither host needs that */
int b_sched_setaffinity(int pid, size_t size, const void *mask)
{
	return 0;
}
