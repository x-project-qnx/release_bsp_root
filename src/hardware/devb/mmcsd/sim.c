/*
 * $QNXLicenseC:
 * Copyright 2007, 2008, QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the QNX Development Suite License Guide at
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */




// Module Description:  MMC sim initialization modulue.

#define EXTERN
#include <sim_mmc.h>
#undef EXTERN

extern int cam_configure( const MODULE_ENTRY *sim_entry, int nsims, int argc, char *argv[] );

const MODULE_ENTRY sim_module = {
	"mmcsd",
	&mmc_sim_args,		// called once for every time it is on the cmd line
	&mmc_sim_attach,	// start setup, and call xpt_bus_register
	&mmc_sim_detach
};

CAM_SIM_ENTRY mmc_sim_entry = {
	&mmc_sim_init,
	&mmc_sim_action
};

// The main routine for the SIM driver
int main(int argc, char *argv[])
{
	// Enable IO capability.
	if (ThreadCtl(_NTO_TCTL_IO, NULL) == -1) {
		perror("ThreadCtl");
		exit(EXIT_FAILURE);
	}

	TAILQ_INIT(&mmc_ctrl.hlist);

	return (cam_configure(&sim_module, 1, argc, argv));
}

SIM_HBA *mmc_alloc_hba(void)
{
	SIM_HBA		*hba;
	SIM_MMC_EXT	*ext;

	if ((hba = sim_alloc_hba(sizeof(SIM_MMC_EXT))) == NULL)
		return (NULL);

	// set some defaults
	ext = (SIM_MMC_EXT *)hba->ext;

	// add hba to drivers hba list
	TAILQ_INSERT_TAIL(&mmc_ctrl.hlist, hba, hlink);
	mmc_ctrl.nhba++;

	return (hba);
}

int mmc_free_hba(SIM_HBA *hba)
{
	SIM_MMC_EXT	*ext;

	mmc_ctrl.nhba--;
	TAILQ_REMOVE(&mmc_ctrl.hlist, hba, hlink);
	ext = (SIM_MMC_EXT *)hba->ext;
	free(ext);
	free(hba);

	return (0);
}

int mmc_sim_args(char *options)
{
	SIM_HBA				*hba;
	SIM_MMC_EXT			*ext;
	char				*value;
	int					opt;
	static char			*opts[] = {
							"clock",	// operation clock
							"pio",
							"bw",
							"bs",
							"noac12",	// disable auto command 12
							"nohs",		// no high speed support
							NULL
						};

	if (*options == '\0') {		// called when command name includes the module name
		if (mmc_ctrl.nhba == 0)
			mmc_ctrl.cflags |= MMC_CFLAG_SCAN;
		return (0);
	}

	if (mmc_ctrl.nhba > MMC_MAX_HBA)	// max hba's reached
		return (-1);

	if ((hba = mmc_alloc_hba()) == NULL)
		return (-1);

	ext = (SIM_MMC_EXT *)hba->ext;
	while (*options != '\0') {
		if ((opt = getsubopt(&options, opts, &value)) == -1) {
			if (sim_drvr_options(hba, value) != EOK)
				continue;
		}
		switch (opt) {
			case 0:
				ext->nclock = strtoull(value, 0, 0);
				ext->eflags |= MMC_EFLAG_FORCECLK;
				break;
			case 1:
				ext->eflags |= MMC_EFLAG_PIO;
				break;
			case 2:
				ext->bus_width = strtoull(value, 0, 0);
				ext->eflags |= MMC_EFLAG_FORCEBW;
				break;
			case 3:
				ext->opts = value;
				break;
			case 4:
				ext->eflags |= MMC_EFLAG_NOAC12;
				break;
			case 5:
				ext->eflags |= MMC_EFLAG_NOHS;
				break;
		}
	}

	return (0);
}

static void *mmc_control_thread(void *data)
{
	struct _pulse	pulse;
	iov_t			iov;
	int				rcvid;
	SIM_HBA			*hba = (SIM_HBA	*)data;
	SIM_MMC_EXT		*ext;

	ext = (SIM_MMC_EXT *)hba->ext;

	SETIOV(&iov, &pulse, sizeof(pulse));

	atomic_set(&ext->eflags, MMC_EFLAG_MONITOR_READY);

	if (ext->detect(hba) != MMC_SUCCESS)
		xpt_async(AC_LOST_DEVICE, hba->pathid, 0, 0, NULL, 0);

	while (1) {
		if ((rcvid = MsgReceivev(mmc_ctrl.chid, &iov, 1, NULL)) == -1)
			continue;

		switch (pulse.code) {
			case MMCSD_PULSE_CARD_INSERTION:
				atomic_set(&ext->eflags, MMC_EFLAG_INSERTED);
				ext->ident = MMCSD_IDENT_START;
				if (mmc_card_ident(hba, 0) == MMC_SUCCESS)
					ext->state = MMCSD_STATE_IDENT;
				break;
			case MMCSD_PULSE_CARD_REMOVAL:
				// No card
				atomic_clr(&ext->eflags, MMC_EFLAG_READY | MMC_EFLAG_INSERTED);
				ext->state = MMCSD_STATE_IDLE;
				xpt_async(AC_LOST_DEVICE, hba->pathid, 0, 0, NULL, 0);
				break;
			case MMCSD_PULSE_CARD_READY:
				if (ext->eflags & MMC_EFLAG_CARD_PRELOAD)
					ext->eflags &= ~MMC_EFLAG_CARD_PRELOAD;
				else
					xpt_async(AC_FOUND_DEVICES, hba->pathid, 0, 0, NULL, 0);
				break;
		}
	}
}

static int
mmc_creat_threads(SIM_HBA *hba)
{
	pthread_attr_t		attr;
	struct sched_param	param;
	SIM_MMC_EXT			*ext;

	ext = (SIM_MMC_EXT *)hba->ext;

	if ((mmc_ctrl.chid = ChannelCreate(_NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK)) == -1 ||
		(mmc_ctrl.coid = ConnectAttach(0, 0, mmc_ctrl.chid, _NTO_SIDE_CHANNEL, 0)) == -1)
	{
		fprintf(stderr, "Channel create failed\n");
		return CAM_FAILURE;
	}

	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	param.sched_priority = 10;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 8192);

	if (pthread_create(&mmc_ctrl.tid, &attr, (void *)mmc_control_thread, hba)) {
		fprintf(stderr, "mmc_create_threads:  Unable to create control thread\n");
		return CAM_FAILURE;
	}

	if ((hba->chid = ChannelCreate(_NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK)) == -1 ||
		(hba->coid = ConnectAttach(0, 0, hba->chid, _NTO_SIDE_CHANNEL, 0)) == -1)
	{
		fprintf(stderr, "Channel create failed\n");
		return CAM_FAILURE;
	}

	// initialize SIM queue routines
	// TODO!!!
	// Adjust the target number
	if ((hba->simq = simq_init(hba->coid, hba, 1, 1, 1, 2, 2, 8)) == NULL) {
		fprintf(stderr, "simq_init failure\n");
		return CAM_FAILURE;
	}

	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	param.sched_priority = 21;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 8192);

	// create the interface thread and wait for it to
	// initialize/attach the interrupt routine
	if (pthread_create(&hba->tid, &attr, (void *)mmc_driver_thread, hba)) {
		fprintf(stderr, "mmc_create_threads:  Unable to create mmcsd control thread\n");
		return CAM_FAILURE;
	}

	if (!(ext->hccap & MMC_HCCAP_DMA))
		atomic_set(&ext->eflags, MMC_EFLAG_PIO);

	if (!(ext->eflags & MMC_EFLAG_PIO)) {
		/*
		 * NOTE: we map the bounce buffer as cached memory because if CPU doesn't support
		 * smart cache, bounce buffer will be allcated in CAM layer.
		 */
		ext->bounce = mmap(0,
			MMCSD_BOUNCE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PHYS | MAP_PRIVATE, NOFD, 0);

		if (ext->bounce == MAP_FAILED)
			return CAM_FAILURE;

		ext->pbounce = (uint8_t *)mphys(ext->bounce);
	}

	while (1) {
		if ((ext->eflags & (MMC_EFLAG_DRIVER_READY | MMC_EFLAG_MONITOR_READY))
						== (MMC_EFLAG_DRIVER_READY | MMC_EFLAG_MONITOR_READY))
			break;
		delay(1);
	}

	if (ext->detect(hba) == MMC_SUCCESS) {
		ext->eflags |= MMC_EFLAG_CARD_PRELOAD;
		atomic_set(&ext->eflags, MMC_EFLAG_INSERTED);
		ext->ident = MMCSD_IDENT_START;
		if (mmc_card_ident(hba, 0) == MMC_SUCCESS) {
			ext->state = MMCSD_STATE_IDENT;
		}
	}

	return (CAM_SUCCESS);
}

int mmc_sim_attach(CAM_ENTRY *centry)
{
	SIM_HBA		*hba;
	SIM_MMC_EXT	*ext;

	if (mmc_ctrl.cflags & MMC_CFLAG_SCAN) {
		if ((hba = mmc_alloc_hba()) == NULL)
			return (CAM_FAILURE);
	}

	// FIXME!!! Currently only one host and one card is supported
	for (hba = TAILQ_FIRST(&mmc_ctrl.hlist); hba; hba = TAILQ_NEXT(hba, hlink)) {
		ext = (SIM_MMC_EXT *)hba->ext;
		if ((bs_init(hba)) != MMC_SUCCESS) {
			fprintf(stderr, "No host controller found\n");
			return (CAM_FAILURE);
		}

		if (ext->eflags & MMC_EFLAG_NOAC12)
			ext->hccap &= ~(MMC_HCCAP_ACMD12);

		mmc_creat_threads(hba);

		atomic_set_value(&ext->eflags, MMC_EFLAG_VALID);
		hba->cam_funcs = centry;
		hba->pathid = xpt_bus_register(&mmc_sim_entry, hba);

		if (hba->pathid > mmc_ctrl.pathid_max)
			mmc_ctrl.pathid_max = hba->pathid;
	}

	return (CAM_SUCCESS);
}

// This function is called by the XPT to request that the SIM be
// initialized.
long mmc_sim_init(SIM_HBA *hba, long path)
{
	SIM_MMC_EXT			*ext;

	ext = (SIM_MMC_EXT *)hba->ext;

	if (atomic_set_value(&ext->eflags, MMC_EFLAG_INITIALIZED) & MMC_EFLAG_INITIALIZED)
		return (CAM_SUCCESS);

	return (CAM_SUCCESS);
}


// This fuction is called when the driver is terminating.  It is provided
// to perform any necessary cleanup before exiting.
int mmc_sim_detach(void)
{
	SIM_HBA			*hba;
	SIM_HBA			*nhba;
	SIM_MMC_EXT		*ext;

	for (hba = TAILQ_FIRST(&mmc_ctrl.hlist); hba; hba = nhba) {
		nhba = TAILQ_NEXT(hba, hlink);
		ext  = (SIM_MMC_EXT *)hba->ext;

		if (hba->verbosity)
			slogf(_SLOGC_SIM_MMC, _SLOG_INFO, "mmc_sim_detach, shutdown HBA %x", (unsigned)hba);

		ext->shutdown(hba);

		if (!(ext->eflags & MMC_EFLAG_PIO))
			munmap(ext->bounce, MMCSD_BOUNCE_SIZE);

		ConnectDetach(hba->coid);
		ChannelDestroy(hba->chid);

		pthread_cancel(hba->tid);
		pthread_join(hba->tid, NULL);

		mmc_free_hba(hba);
	}

	return (0);
}

// This function is provided so that the peripheral driver can
// enqueue an I/O command.
int mmc_scsi_io(SIM_HBA *hba, CCB_SCSIIO *ccb)
{
	mmc_io_cmds(hba);

	return (CAM_REQ_INPROG);
}

// This function is used to reset the specified SCSI bus.  This
// request shall slways result in the SCSI RST signal being
// asserted.
int mmc_reset_bus(SIM_HBA *hba, CCB_RESETBUS *ccb)
{
	simq_scsi_reset(hba->simq);
	xpt_async(AC_BUS_RESET, hba->pathid, -1, -1, NULL, 0);
	return (CAM_REQ_CMP);
}

// This function is used to reset the specified SCSI target.  This
// function should not be used in normal operation, but if I/O to a
// particular device hangs up for some reason, drivers can abort the
// I/O and reset the device before trying again.  This request shall
// always result in a bus device reset message being issued over SCSI.
int mmc_reset_dev(SIM_HBA *hba, CCB_RESETDEV *ccb)
{
	simq_reset_dev(hba->simq, ccb);
	xpt_async(AC_SENT_BDR, hba->pathid, -1, -1, NULL, 0);
	return (CAM_REQ_CMP);
}

// This function is provided so that the peripheral driver can
// release a frozen SIM queue for the selected Logical Unit.
static int mmc_rel_simq(SIM_HBA *hba, CCB_RELSIM *ccb)
{
	simq_rel_simq(hba->simq, ccb);
	if (MsgSendPulse(hba->coid, SIM_PRIORITY, SIM_ENQUEUE, 0) == -1) {
	}
	return (CAM_REQ_CMP);
}

// This function requests that a SCSI I/O request be terminated by
// identifying the CCB associated with the request.  This request
// does not necessarily result in a terminated I/O process message
// being issued over SCSI.
static int mmc_term_io(SIM_HBA *hba, CCB_TERMIO *ccb) 
{
	hba = hba, ccb = ccb;
	return (CAM_REQ_CMP_ERR);
}

// This function is used to get information on the installed HBA
// hardware, including number of HBAs installed
static int mmc_path_inq(SIM_HBA *hba, CCB_PATHINQ *ccb)
{
	SIM_MMC_EXT	*ext;

	ext = (SIM_MMC_EXT *)hba->ext;

	ccb->cam_version_num	= CAM_VERSION;
	ccb->cam_initiator_id	= 7;
	ccb->cam_hba_inquiry	= 0;
	ccb->cam_target_sprt	= 0;
	ccb->cam_hba_misc		= 0;
	ccb->cam_hba_eng_cnt	= 0;
	ccb->cam_sim_priv		= SIM_PRIV;
	ccb->cam_async_flags	= AC_BUS_RESET;
	memset(ccb->cam_vuhba_flags, 0x00, sizeof(*ccb->cam_vuhba_flags));
	if (ext->eflags & MMC_EFLAG_PIO)
		ccb->cam_vuhba_flags[CAM_VUHBA_FLAGS]	= CAM_VUHBA_FLAG_PTR;
	else
		ccb->cam_vuhba_flags[CAM_VUHBA_FLAGS]	= CAM_VUHBA_FLAG_PTR | CAM_VUHBA_FLAG_DMA;
	ccb->cam_vuhba_flags[CAM_VUHBA_MAX_SG]		= MMC_MAX_SG;
	ccb->cam_vuhba_flags[CAM_VUHBA_MAX_LINKED]	= 1;
	strcpy(ccb->cam_sim_vid, "QSS MMCSD");
	strncpy(ccb->cam_hba_vid, hba->cfg.Description, sizeof(ccb->cam_hba_vid));

	return (CAM_REQ_CMP);
}

//  All CCB requests to the SIM are made through this function.
long mmc_sim_action(SIM_HBA *hba, CCB *ccb_ptr)
{
	CCB_HEADER	*ccb = (CCB_HEADER *)ccb_ptr;
	int			cam_status;

	if (ccb->cam_target_id >= MMC_MAX_TARGET) {
		ccb->cam_status = CAM_TID_INVALID;
		return (CAM_FAILURE);
	}

	if (ccb->cam_flags & (CAM_CDB_PHYS | CAM_NXT_CCB_PHYS |
			CAM_CALLBCK_PHYS | CAM_SNS_BUF_PHYS)) {
		ccb->cam_status = CAM_NO_HBA;
		return (CAM_FAILURE);
	}

	cam_status	= CAM_REQ_CMP;

	switch (ccb->cam_func_code) {
		case XPT_NOOP:
			break;
		case XPT_SCSI_IO:
		case XPT_RESET_BUS:
		case XPT_RESET_DEV:
		case XPT_ABORT:
		case XPT_DEVCTL:
			cam_status = CAM_REQ_INPROG;
			break;
		case XPT_REL_SIMQ:
			cam_status = mmc_rel_simq(hba, (CCB_RELSIM *)ccb);
			break;
		case XPT_TERM_IO:
			cam_status = mmc_term_io(hba, (CCB_TERMIO *)ccb);
			break;
		case XPT_PATH_INQ:
			cam_status = mmc_path_inq(hba, (CCB_PATHINQ *)ccb);
			break;
		case XPT_GDEV_TYPE:
		case XPT_SDEV_TYPE:
		case XPT_SASYNC_CB:
			// These are not serviced by the SIM, the XPT should do them
			cam_status = CAM_REQ_INVALID;
			break;

		case XPT_EN_LUN:
		case XPT_TARGET_IO:
			cam_status = CAM_FUNC_NOTAVAIL;
			break;

		case XPT_VUNIQUE:
			// no special MMC commands
			cam_status = CAM_REQ_INVALID;
			break;

		case XPT_ENG_INQ:
		case XPT_ENG_EXEC:
		default:
			cam_status = CAM_REQ_INVALID;
			break;
	}

	if ((ccb->cam_status = cam_status) == CAM_REQ_INPROG) {
		simq_ccb_enqueue(hba->simq, (CCB_SCSIIO *)ccb);
		if (MsgSendPulse(hba->coid, SIM_PRIORITY, SIM_ENQUEUE, 0) == -1) {
		}
	}

	return (CAM_SUCCESS);
}

__SRCVERSION("sim.c $Rev: 153421 $");
