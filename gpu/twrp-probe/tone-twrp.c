/*
 * tone-twrp: audio path test in TWRP on Nokia's kernel (ADSP + modem started by hand, pd-mapper running,
 * so the sdm660-snd-card-drg card exists). Our own code over the Linux ALSA ioctl ABI (<sound/asound.h>).
 *   tone-twrp list              all mixer control names (and current integer values)
 *   tone-twrp play [ctl=v ...]  set controls by name (Nokia's mixer_paths_drg.xml speaker route:
 *                               "TERT_MI2S_RX Audio Mixer MultiMedia1=1"), then 2 s of 1 kHz on pcmC0D0p
 *                               (MultiMedia1, 48 kHz, stereo, S16_LE) at 1/4 of full scale
 */
#include <linux/ioctl.h>
#include <sound/asound.h>

extern void __libc_init(void *raw_args, void (*onexit)(void), int (*slingshot)(int, char **, char **),
			const void *structors);
extern int printf(const char *, ...);
extern int fflush(void *);
extern int open(const char *, int, ...);
extern int close(int);
extern int ioctl(int, int, ...);
extern int *__errno(void);
extern void *memset(void *, int, size_t);
extern void *calloc(size_t, size_t);
extern size_t strlen(const char *);
extern int strncmp(const char *, const char *, size_t);
extern char *strchr(const char *, int);
extern long strtol(const char *, char **, int);
#define O_RDWR 2

static int ctl = -1;

static int list(void)
{
	struct snd_ctl_elem_list l;
	memset(&l, 0, sizeof(l));
	if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_LIST, &l))
		return printf("ELEM_LIST errno %d\n", *__errno()), 1;
	unsigned n = l.count;
	struct snd_ctl_elem_id *ids = calloc(n, sizeof(*ids));
	l.offset = 0, l.space = n, l.pids = ids;
	if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_LIST, &l))
		return printf("ELEM_LIST(2) errno %d\n", *__errno()), 1;
	printf("%u controls\n", l.used);
	for (unsigned i = 0; i < l.used; i++) {
		struct snd_ctl_elem_info inf;
		memset(&inf, 0, sizeof(inf));
		inf.id = ids[i];
		int t = ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &inf) ? -1 : (int)inf.type;
		long v = 0;
		if (t == SNDRV_CTL_ELEM_TYPE_INTEGER || t == SNDRV_CTL_ELEM_TYPE_BOOLEAN || t == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
			struct snd_ctl_elem_value val;
			memset(&val, 0, sizeof(val));
			val.id = ids[i];
			if (!ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &val))
				v = t == SNDRV_CTL_ELEM_TYPE_ENUMERATED ? (long)val.value.enumerated.item[0] : val.value.integer.value[0];
		}
		printf("%u type %d count %u value %ld: %s\n", ids[i].numid, t, t >= 0 ? inf.count : 0, v, ids[i].name);
	}
	return 0;
}

/* "name=value": integer/boolean value, or an enumerated item by its text */
static int set(const char *arg)
{
	const char *eq = strchr(arg, '=');
	if (!eq)
		return printf("bad control %s\n", arg), 1;
	struct snd_ctl_elem_info inf;
	memset(&inf, 0, sizeof(inf));
	inf.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	size_t nl = (size_t)(eq - arg);
	if (nl >= sizeof(inf.id.name))
		return 1;
	for (size_t i = 0; i < nl; i++)
		inf.id.name[i] = (unsigned char)arg[i];
	if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &inf))
		return printf("control not found: %.*s (errno %d)\n", (int)nl, arg, *__errno()), 1;
	struct snd_ctl_elem_value val;
	memset(&val, 0, sizeof(val));
	val.id = inf.id;
	const char *vs = eq + 1;
	if (inf.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned found = 0xffffffffu;
		for (unsigned k = 0; k < inf.value.enumerated.items; k++) {
			struct snd_ctl_elem_info ei = inf;
			ei.value.enumerated.item = k;
			if (!ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) &&
			    !strncmp(ei.value.enumerated.name, vs, sizeof(ei.value.enumerated.name)))
				found = k;
		}
		if (found == 0xffffffffu)
			found = (unsigned)strtol(vs, 0, 0);
		for (unsigned c = 0; c < inf.count; c++)
			val.value.enumerated.item[c] = found;
	} else {
		long v = strtol(vs, 0, 0);
		for (unsigned c = 0; c < inf.count && c < 128; c++)
			val.value.integer.value[c] = v;
	}
	int rc = ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &val);
	printf("set %s: %s\n", arg, rc ? "FAILED" : "ok");
	return rc != 0;
}

static void mask_set(struct snd_pcm_hw_params *p, int which, unsigned bit)
{
	struct snd_mask *m = &p->masks[which - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m, 0, sizeof(*m));
	m->bits[bit / 32] = 1u << (bit % 32);
}

static void interval_set(struct snd_pcm_hw_params *p, int which, unsigned v)
{
	struct snd_interval *i = &p->intervals[which - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	i->min = i->max = v;
	i->integer = 1;
}

static void interval_any(struct snd_pcm_hw_params *p, int which)
{
	struct snd_interval *i = &p->intervals[which - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	i->min = 0;
	i->max = 0xffffffffu;
}

static short buf[48000 * 2 * 2];

static int play(void)
{
	int pcm = open("/dev/snd/pcmC0D0p", O_RDWR);
	if (pcm < 0)
		return printf("open pcmC0D0p errno %d\n", *__errno()), 1;
	struct snd_pcm_hw_params hw;
	memset(&hw, 0, sizeof(hw));
	for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
		memset(&hw.masks[k - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof(struct snd_mask));
	for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++)
		interval_any(&hw, k);
	mask_set(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	mask_set(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_RATE, 48000);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 960);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIODS, 4);
	hw.rmask = ~0u;
	if (ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &hw))
		return printf("HW_PARAMS errno %d\n", *__errno()), 1;
	printf("hw_params ok: rate %u channels %u period %u periods %u\n",
	       hw.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
	       hw.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
	       hw.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
	       hw.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min);
	if (ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE))
		return printf("PREPARE errno %d\n", *__errno()), 1;
	/* 1 kHz at 48 kHz: 48 samples per cycle; a triangle-free table from a parabola approximation of sine */
	for (int i = 0; i < 48000 * 2; i++) {
		int ph = i % 48;	/* 0..47 */
		int x = ph < 24 ? ph : ph - 24;	/* half cycle */
		int s = 4 * x * (24 - x);	/* 0..576, peak at 12 */
		int v = s * 8192 / 576;		/* 1/4 full scale */
		if (ph >= 24)
			v = -v;
		buf[2 * i] = buf[2 * i + 1] = (short)v;
	}
	struct snd_xferi x = { 0, buf, 48000 * 2 };
	int rc = ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x);
	printf("write: rc %d errno %d frames %ld\n", rc, rc ? *__errno() : 0, (long)x.result);
	rc = ioctl(pcm, SNDRV_PCM_IOCTL_DRAIN);
	printf("drain: rc %d\n", rc);
	close(pcm);
	return 0;
}

static int main_(int argc, char **argv, char **envp)
{
	(void)envp;
	ctl = open("/dev/snd/controlC0", O_RDWR);
	if (ctl < 0)
		return printf("open controlC0 errno %d\n", *__errno()), 1;
	int rc = 0;
	if (argc > 1 && !strncmp(argv[1], "list", 5)) {
		rc = list();
	} else if (argc > 1 && !strncmp(argv[1], "play", 5)) {
		for (int i = 2; i < argc; i++)
			rc |= set(argv[i]);
		rc |= play();
	} else {
		printf("usage: tone-twrp list | play [ctl=v ...]\n");
		rc = 2;
	}
	printf("RESULT: %s\n", rc ? "FAIL" : "OK");
	fflush(0);
	return rc;
}

__attribute__((used)) static void start_c(void *raw_args)
{
	static void *structors[6];
	__libc_init(raw_args, 0, main_, structors);
}
__asm__(".text\n.globl _start\n.type _start,%function\n_start:\n"
	"mov x29, #0\nmov x30, #0\nmov x0, sp\nb start_c\n");
