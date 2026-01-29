/*
 * Copyright (c) 2026 Ron Manosuthi <rman401@proton.me>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/task.h>
#include <sys/atomic.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ic/bcm2835_clock.h>
#include <dev/ic/bcm2835_mbox.h>
#include <dev/ic/bcm2835_vcprop.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

#define DEVNAME(sc) ((sc)->sc_dev.dv_xname)
#define HZ_TO_MHZ(hz) ((hz) / 1000000)

struct rpicpu_softc {
	struct device	sc_dev;

	uint32_t	sc_min_clk_hz;
	uint32_t	sc_max_clk_hz;
	uint32_t	sc_target_clk_hz;

	struct task	sc_task_reclk;
};

static struct rpicpu_softc *rpicpu_sc;

int	rpicpu_match(struct device *, void *, void *);
void	rpicpu_attach(struct device *, struct device *, void *);
int	rpicpu_get_board_rev(uint32_t *);
int	rpicpu_clockspeed(int *);
void	rpicpu_setperf(int);
void	rpicpu_reclock(void *);

const struct cfattach rpicpu_ca = {
	sizeof (struct rpicpu_softc), rpicpu_match, rpicpu_attach
};

struct cfdriver rpicpu_cd = {
	NULL, "rpicpu", DV_DULL
};

int
rpicpu_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	/* XXX find a better node */
	return OF_is_compatible(faa->fa_node, "raspberrypi,bcm2835-firmware");
}

void
rpicpu_attach(struct device *parent, struct device *self, void *aux)
{
	struct rpicpu_softc *sc = (struct rpicpu_softc *)self;
	int error;
	uint32_t board_rev;

	error = rpicpu_get_board_rev(&board_rev);
	if (error) {
		printf(": failed to get board revision: %d\n", error);
		return;
	}
	board_rev &= VCPROP_REV_MODEL;
	board_rev >>= 4;
	if (board_rev != RPI_MODEL_4B &&
	    board_rev != RPI_MODEL_400) {
		printf(": unsupported board revision %#x\n", board_rev);
		return;
	}

	error = bcmclock_get_frequency(&sc->sc_min_clk_hz,
	    VCPROP_CLK_ARM, VCPROPTAG_GET_MIN_CLOCKRATE);
	if (error) {
		printf(": failed to get min clock frequency: %d\n", error);
		return;
	}

	error = bcmclock_get_frequency(&sc->sc_max_clk_hz,
	    VCPROP_CLK_ARM, VCPROPTAG_GET_MAX_CLOCKRATE);
	if (error) {
		printf(": failed to get max clock frequency: %d\n", error);
		return;
	}

	cpu_cpuspeed = rpicpu_clockspeed;
	cpu_setperf = rpicpu_setperf;

	task_set(&sc->sc_task_reclk, rpicpu_reclock, sc);
	rpicpu_sc = sc;

	printf(": %d-%d MHz\n",
	    HZ_TO_MHZ(sc->sc_min_clk_hz),
	    HZ_TO_MHZ(sc->sc_max_clk_hz));

	/* XXX sensordev */
}

int
rpicpu_get_board_rev(uint32_t *board_rev)
{
	struct request {
		struct vcprop_buffer_hdr vb_hdr;
		struct vcprop_tag_boardrev vbt_br;
		struct vcprop_tag end;
	} __packed;

	int error;
	uint32_t result;
	struct request req = {
		.vb_hdr = {
			.vpb_len = sizeof(req),
			.vpb_rcode = VCPROP_PROCESS_REQUEST,
		},
		.vbt_br = {
			.tag = {
				.vpt_tag = VCPROPTAG_GET_BOARDREVISION,
				.vpt_len = VCPROPTAG_LEN(req.vbt_br),
				.vpt_rcode = VCPROPTAG_REQUEST
			},
			.rev = 0,
		},
		.end = {
			.vpt_tag = VCPROPTAG_NULL
		}
	};

	error = bcmmbox_post(BCMMBOX_CHANARM2VC, &req, sizeof(req), &result);
	if (error) {
		printf("%s: post failed, error %d\n", __func__, error);
		return error;
	}

	if (vcprop_tag_success_p(&req.vbt_br.tag)) {
		*board_rev = req.vbt_br.rev;
		return 0;
	} else {
		return (EINVAL);
	}
}

int
rpicpu_clockspeed(int *freq)
{
	struct rpicpu_softc *sc = rpicpu_sc;
	uint32_t clk_hz;

	if (sc == NULL)
		return (ENXIO);

	clk_hz = atomic_load_int(&sc->sc_target_clk_hz);
	*freq = HZ_TO_MHZ(clk_hz);
	return 0;
}

void
rpicpu_setperf(int level)
{
	struct rpicpu_softc *sc = rpicpu_sc;
	uint64_t target_clk_hz, min_hz, max_hz;

	if (sc == NULL)
		return;

	min_hz = sc->sc_min_clk_hz;
	max_hz = sc->sc_max_clk_hz;
	target_clk_hz = min_hz + (((max_hz - min_hz) * level) / 100);
	atomic_store_int(&sc->sc_target_clk_hz, (uint32_t)target_clk_hz);

	/* Defer work to task queue so bcmmbox_post(9) can sleep. */
	task_add(systqmp, &sc->sc_task_reclk);
}

void
rpicpu_reclock(void *cookie)
{
	struct rpicpu_softc *sc = (struct rpicpu_softc *)cookie;
	uint32_t target_clk_hz;

	target_clk_hz = atomic_load_int(&sc->sc_target_clk_hz);
	KASSERT(target_clk_hz >= sc->sc_min_clk_hz &&
	    target_clk_hz <= sc->sc_max_clk_hz);

	bcmclock_set_frequency(VCPROP_CLK_ARM, target_clk_hz);
}
