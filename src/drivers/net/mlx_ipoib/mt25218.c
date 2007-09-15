/**************************************************************************
Etherboot -  BOOTP/TFTP Bootstrap Program
Skeleton NIC driver for Etherboot
***************************************************************************/

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 */

#include <errno.h>
#include <gpxe/pci.h>
#include <gpxe/iobuf.h>
#include <gpxe/netdevice.h>
#include <gpxe/infiniband.h>

/* to get some global routines like printf */
#include "etherboot.h"
/* to get the interface to the body of the program */
#include "nic.h"

#include "mt25218_imp.c"

#include "arbel.h"

struct arbel_send_work_queue {
	/** Doorbell record number */
	unsigned int doorbell_idx;
	/** Work queue entries */
	//	struct ud_send_wqe_st *wqe;
	union ud_send_wqe_u *wqe_u;
};

struct arbel_completion_queue {
	/** Doorbell record number */
	unsigned int doorbell_idx;
	/** Completion queue entries */
	union cqe_st *cqe;
};

struct arbel {
	/** User Access Region */
	void *uar;
	/** Doorbell records */
	union db_record_st *db_rec;
};



struct mlx_nic {
	/** Queue pair handle */
	udqp_t ipoib_qph;
	/** Broadcast Address Vector */
	ud_av_t bcast_av;
	/** Send completion queue */
	cq_t snd_cqh;
	/** Receive completion queue */
	cq_t rcv_cqh;
};

/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int mlx_open ( struct net_device *netdev ) {

	( void ) netdev;

	return 0;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
static void mlx_close ( struct net_device *netdev ) {

	( void ) netdev;

}

#warning "Broadcast address?"
static uint8_t ib_broadcast[IB_ALEN] = { 0xff, };


/**
 * Transmit packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int mlx_transmit ( struct net_device *netdev,
			  struct io_buffer *iobuf ) {
	struct mlx_nic *mlx = netdev->priv;
	ud_send_wqe_t snd_wqe;
	int rc;

	snd_wqe = alloc_send_wqe ( mlx->ipoib_qph );
	if ( ! snd_wqe ) {
		DBGC ( mlx, "MLX %p out of TX WQEs\n", mlx );
		return -ENOBUFS;
	}

	prep_send_wqe_buf ( mlx->ipoib_qph, mlx->bcast_av, snd_wqe,
			    iobuf->data, 0, iob_len ( iobuf ), 0 );
	if ( ( rc = post_send_req ( mlx->ipoib_qph, snd_wqe, 1 ) ) != 0 ) {
		DBGC ( mlx, "MLX %p could not post TX WQE %p: %s\n",
		       mlx, snd_wqe, strerror ( rc ) );
		free_wqe ( snd_wqe );
		return rc;
	}

	return 0;
}

static int arbel_post_send ( struct ib_device *ibdev, struct io_buffer *iobuf,
			     struct ib_address_vector *av,
			     struct ib_queue_pair *qp );

static struct io_buffer *tx_ring[NUM_IPOIB_SND_WQES];
static int next_tx_idx = 0;

static int mlx_transmit_direct ( struct net_device *netdev,
				 struct io_buffer *iobuf ) {
	struct mlx_nic *mlx = netdev->priv;
	int rc;

	struct arbel arbel = {
		.uar = memfree_pci_dev.uar,
		.db_rec = dev_ib_data.uar_context_base,
	};
	struct arbel_send_work_queue arbel_send_queue = {
		.doorbell_idx = IPOIB_SND_QP_DB_IDX,
		.wqe_u = ( (struct udqp_st *) mlx->ipoib_qph )->snd_wq,
	};
	struct ib_device ibdev = {
		.priv = &arbel,
	};
	struct ib_queue_pair qp = {
		.qpn = ib_get_qpn ( mlx->ipoib_qph ),
		.send = {
			.num_wqes = NUM_IPOIB_SND_WQES,
			.next_idx = next_tx_idx,
			.iobufs = tx_ring,
			.priv = &arbel_send_queue,
		},
	};
	struct ud_av_st *bcast_av = mlx->bcast_av;
	struct arbelprm_ud_address_vector *bav =
		( struct arbelprm_ud_address_vector * ) &bcast_av->av;
	struct ib_address_vector av = {
		.dest_qp = bcast_av->dest_qp,
		.qkey = bcast_av->qkey,
		.dlid = MLX_EXTRACT ( bav, rlid ),
		.rate = ( MLX_EXTRACT ( bav, max_stat_rate ) ? 1 : 4 ),
		.sl = MLX_EXTRACT ( bav, sl ),
		.gid_present = 1,
	};
	memcpy ( &av.gid, ( ( void * ) bav ) + 16, 16 );

	rc = arbel_post_send ( &ibdev, iobuf, &av, &qp );

	next_tx_idx = qp.send.next_idx;

	return rc;
}


/**
 * Handle TX completion
 *
 * @v netdev		Network device
 * @v ib_cqe		Completion queue entry
 */
static void mlx_tx_complete ( struct net_device *netdev,
			      struct ib_cqe_st *ib_cqe ) {
	netdev_tx_complete_next_err ( netdev,
				      ( ib_cqe->is_error ? -EIO : 0 ) );
}

/**
 * Handle RX completion
 *
 * @v netdev		Network device
 * @v ib_cqe		Completion queue entry
 */
static void mlx_rx_complete ( struct net_device *netdev,
			      struct ib_cqe_st *ib_cqe ) {
	unsigned int len;
	struct io_buffer *iobuf;
	void *buf;

	/* Check for errors */
	if ( ib_cqe->is_error ) {
		netdev_rx_err ( netdev, NULL, -EIO );
		return;
	}

	/* Allocate I/O buffer */
	len = ( ib_cqe->count - GRH_SIZE );
	iobuf = alloc_iob ( len );
	if ( ! iobuf ) {
		netdev_rx_err ( netdev, NULL, -ENOMEM );
		return;
	}

	/* Fill I/O buffer */
	buf = get_rcv_wqe_buf ( ib_cqe->wqe, 1 );
	memcpy ( iob_put ( iobuf, len ), buf, len );

	/* Hand off to network stack */
	netdev_rx ( netdev, iobuf );
}

/**
 * Poll completion queue
 *
 * @v netdev		Network device
 * @v cq		Completion queue
 * @v handler		Completion handler
 */
static void mlx_poll_cq ( struct net_device *netdev, cq_t cq,
			  void ( * handler ) ( struct net_device *netdev,
					       struct ib_cqe_st *ib_cqe ) ) {
	struct mlx_nic *mlx = netdev->priv;
	struct ib_cqe_st ib_cqe;
	uint8_t num_cqes;

	while ( 1 ) {

		/* Poll for single completion queue entry */
		ib_poll_cq ( cq, &ib_cqe, &num_cqes );

		/* Return if no entries in the queue */
		if ( ! num_cqes )
			return;

		DBGC ( mlx, "MLX %p cpl in %p: err %x send %x "
		       "wqe %p count %lx\n", mlx, cq, ib_cqe.is_error,
		       ib_cqe.is_send, ib_cqe.wqe, ib_cqe.count );

		/* Handle TX/RX completion */
		handler ( netdev, &ib_cqe );

		/* Free associated work queue entry */
		free_wqe ( ib_cqe.wqe );
	}
}

/**
 * Poll for completed and received packets
 *
 * @v netdev		Network device
 */
static void mlx_poll ( struct net_device *netdev ) {
	struct mlx_nic *mlx = netdev->priv;
	int rc;

	if ( ( rc = poll_error_buf() ) != 0 ) {
		DBG ( "poll_error_buf() failed: %s\n", strerror ( rc ) );
		return;
	}

	/* Drain event queue.  We can ignore events, since we're going
	 * to just poll all completion queues anyway.
	 */
	if ( ( rc = drain_eq() ) != 0 ) {
		DBG ( "drain_eq() failed: %s\n", strerror ( rc ) );
		return;
	}

	/* Poll completion queues */
	mlx_poll_cq ( netdev, mlx->snd_cqh, mlx_tx_complete );
	mlx_poll_cq ( netdev, mlx->rcv_cqh, mlx_rx_complete );
}

/**
 * Enable or disable interrupts
 *
 * @v netdev		Network device
 * @v enable		Interrupts should be enabled
 */
static void mlx_irq ( struct net_device *netdev, int enable ) {

	( void ) netdev;
	( void ) enable;

}

static struct net_device_operations mlx_operations = {
	.open		= mlx_open,
	.close		= mlx_close,
#if 0
	.transmit	= mlx_transmit,
#else
	.transmit	= mlx_transmit_direct,
#endif
	.poll		= mlx_poll,
	.irq		= mlx_irq,
};



static struct ib_gid arbel_no_gid = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 }
};

/**
 * Ring doorbell register in UAR
 *
 * @v arbel		Arbel device
 * @v db_reg		Doorbell register structure
 * @v offset		Address of doorbell
 */
static void arbel_ring_doorbell ( struct arbel *arbel, void *db_reg,
				  unsigned int offset ) {
	uint32_t *db_reg_dword = db_reg;

	DBG ( "arbel_ring_doorbell %08lx:%08lx to %lx\n",
	      db_reg_dword[0], db_reg_dword[1],
	      virt_to_phys ( arbel->uar + offset ) );

	barrier();
	writel ( db_reg_dword[0], ( arbel->uar + offset + 0 ) );
	barrier();
	writel ( db_reg_dword[1], ( arbel->uar + offset + 4 ) );
}

/**
 * Post send work queue entry
 *
 * @v ibdev		Infiniband device
 * @v iobuf		I/O buffer
 * @v av		Address vector
 * @v qp		Queue pair
 * @ret rc		Return status code
 */
static int arbel_post_send ( struct ib_device *ibdev, struct io_buffer *iobuf,
			     struct ib_address_vector *av,
			     struct ib_queue_pair *qp ) {
	struct arbel *arbel = ibdev->priv;
	struct ib_work_queue *wq = &qp->send;
	struct arbel_send_work_queue *arbel_wq = wq->priv;
	unsigned int wqe_idx_mask = ( wq->num_wqes - 1 );
	struct ud_send_wqe_st *prev_wqe;
	struct ud_send_wqe_st *wqe;
	struct ib_gid *gid;
	size_t nds;
	union db_record_st *db_rec;
	struct send_doorbell_st db_reg;

	/* Allocate work queue entry */
	if ( wq->iobufs[wq->next_idx & wqe_idx_mask] ) {
		DBGC ( arbel, "ARBEL %p send queue full", arbel );
		return -ENOBUFS;
	}
	wq->iobufs[wq->next_idx & wqe_idx_mask] = iobuf;
	prev_wqe = &arbel_wq->wqe_u[(wq->next_idx - 1) & wqe_idx_mask].wqe_cont.wqe;
	wqe = &arbel_wq->wqe_u[wq->next_idx & wqe_idx_mask].wqe_cont.wqe;

	/* Construct work queue entry */
	MLX_POPULATE_1 ( &wqe->next.next, arbelprm_wqe_segment_next_st, 1,
			 always1, 1 );
	memset ( &wqe->next.control, 0,
		 sizeof ( wqe->next.control ) );
	MLX_POPULATE_1 ( &wqe->next.control,
			 arbelprm_wqe_segment_ctrl_send_st, 0,
			 always1, 1 );
	memset ( &wqe->udseg, 0, sizeof ( wqe->udseg ) );
	MLX_POPULATE_2 ( &wqe->udseg, arbelprm_ud_address_vector_st, 0,
			 pd, GLOBAL_PD,
			 port_number, PXE_IB_PORT );
	MLX_POPULATE_2 ( &wqe->udseg, arbelprm_ud_address_vector_st, 1,
			 rlid, av->dlid,
			 g, av->gid_present );
	MLX_POPULATE_2 ( &wqe->udseg, arbelprm_ud_address_vector_st, 2,
			 max_stat_rate, ( ( av->rate >= 3 ) ? 0 : 1 ),
			 msg, 3 );
	MLX_POPULATE_1 ( &wqe->udseg, arbelprm_ud_address_vector_st, 3,
			 sl, av->sl );
	gid = ( av->gid_present ? &av->gid : &arbel_no_gid );
	memcpy ( ( ( ( void * ) &wqe->udseg ) + 16 ),
		 gid, sizeof ( *gid ) );
	MLX_POPULATE_1 ( &wqe->udseg, arbelprm_wqe_segment_ud_st, 8,
			 destination_qp, av->dest_qp );
	MLX_POPULATE_1 ( &wqe->udseg, arbelprm_wqe_segment_ud_st, 9,
			 q_key, av->qkey );
	wqe->mpointer[0].local_addr_l =
		cpu_to_be32 ( virt_to_bus ( iobuf->data ) );
	wqe->mpointer[0].byte_count = cpu_to_be32 ( iob_len ( iobuf ) );

	DBG ( "Work queue entry:\n" );
	DBG_HD ( wqe, sizeof ( *wqe ) );

	/* Update previous work queue entry's "next" field */
	nds = ( ( offsetof ( typeof ( *wqe ), mpointer ) +
		  sizeof ( wqe->mpointer[0] ) ) >> 4 );
	MLX_MODIFY ( &prev_wqe->next.next, arbelprm_wqe_segment_next_st, 0,
		     nopcode, XDEV_NOPCODE_SEND );
	MLX_POPULATE_3 ( &prev_wqe->next.next, arbelprm_wqe_segment_next_st, 1,
			 nds, nds,
			 f, 1,
			 always1, 1 );

	DBG ( "Previous work queue entry's next field:\n" );
	DBG_HD ( &prev_wqe->next.next, sizeof ( prev_wqe->next.next ) );

	/* Update doorbell record */
	db_rec = &arbel->db_rec[arbel_wq->doorbell_idx];
	MLX_POPULATE_1 ( db_rec, arbelprm_qp_db_record_st, 0, 
			 counter, ( ( wq->next_idx + 1 ) & 0xffff ) );
	barrier();
	DBG ( "Doorbell record:\n" );
	DBG_HD ( db_rec, 8 );

	/* Ring doorbell register */
	MLX_POPULATE_4 ( &db_reg, arbelprm_send_doorbell_st, 0,
			 nopcode, XDEV_NOPCODE_SEND,
			 f, 1,
			 wqe_counter, ( wq->next_idx & 0xffff ),
			 wqe_cnt, 1 );
	MLX_POPULATE_2 ( &db_reg, arbelprm_send_doorbell_st, 1,
			 nds, nds,
			 qpn, qp->qpn );
	arbel_ring_doorbell ( arbel, &db_reg, POST_SND_OFFSET );

	/* Update work queue's index */
	wq->next_idx++;

	return 0;
}

static void arbel_parse_completion ( struct arbel *arbel,
				     union cqe_st *cqe,
				     struct ib_completion *completion ) {
	memset ( completion, 0, sizeof ( *completion ) );
	is_send = MLX_EXTRACT ( cqe, arbelprm_completion_queue_entry_st, s );					
	completion->len =
		MLX_EXTRACT ( cqe, arbelprm_completion_queue_entry_st,
			      byte_cnt );}

/**
 * Poll completion queue
 *
 * @v ibdev		Infiniband device
 * @v cq		Completion queue
 * @v complete		Completion handler
 */
static void arbel_poll_cq ( struct ib_device *ibdev,
			    struct ib_completion_queue *cq,
			    ib_completer_t complete_send,
			    ib_completer_t complete_recv ) {
	struct arbel *arbel = ibdev->priv;
	struct arbel_completion_queue *arbel_cq = cq->priv;
	unsigned int cqe_idx_mask = ( cq->num_cqes - 1 );
	union db_record_st *db_rec = &arbel->db_rec[arbel_cq->doorbell_idx];
	union cqe_st *cqe;
	struct ib_completion completion;
	struct io_buffer *iobuf;
	int is_send;

	while ( 1 ) {
		/* Look for completion entry */
		cqe = &arbel_cq->cqe[cq->next_idx & cqe_idx_mask];
		if ( MLX_EXTRACT ( cqe, arbelprm_completion_queue_entry_st,
				   owner ) != 0 ) {
			/* Entry still owned by hardware; end of poll */
			break;
		}

		/* Parse completion */

		
		
		/* Handle completion */
		( is_send ? complete_send : complete_recv ) ( ibdev,
							      &completion,
							      iobuf );

		/* Return ownership to hardware */
		MLX_POPULATE_1 ( cqe, arbelprm_completion_queue_entry_st, 7,
				 owner, 1 );
		barrier();
		/* Update completion queue's index */
		cq->next_idx++;
		/* Update doorbell record */
		MLX_POPULATE_1 ( db_rec, arbelprm_cq_ci_db_record_st, 0,
				 counter, ( cq->next_idx & 0xffffffffUL ) );
	}
}

/** Arbel Infiniband operations */
static struct ib_device_operations arbel_ib_operations = {
	.post_send	= arbel_post_send,
	.poll_cq	= arbel_poll_cq,
};

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void arbel_remove ( struct pci_device *pci ) {
	struct net_device *netdev = pci_get_drvdata ( pci );

	unregister_netdev ( netdev );
	ib_driver_close ( 0 );
	netdev_nullify ( netdev );
	netdev_put ( netdev );
}

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @v id		PCI ID
 * @ret rc		Return status code
 */
static int arbel_probe ( struct pci_device *pci,
			 const struct pci_device_id *id __unused ) {
	struct net_device *netdev;
	struct mlx_nic *mlx;
	struct ib_mac *mac;
	udqp_t qph;
	int rc;

	/* Allocate net device */
	netdev = alloc_ibdev ( sizeof ( *mlx ) );
	if ( ! netdev )
		return -ENOMEM;
	netdev_init ( netdev, &mlx_operations );
	mlx = netdev->priv;
	pci_set_drvdata ( pci, netdev );
	netdev->dev = &pci->dev;
	memset ( mlx, 0, sizeof ( *mlx ) );

	/* Fix up PCI device */
	adjust_pci_device ( pci );

	/* Initialise hardware */
	if ( ( rc = ib_driver_init ( pci, &qph ) ) != 0 )
		goto err_ipoib_init;
	mlx->ipoib_qph = qph;
	mlx->bcast_av = ib_data.bcast_av;
	mlx->snd_cqh = ib_data.ipoib_snd_cq;
	mlx->rcv_cqh = ib_data.ipoib_rcv_cq;
	mac = ( ( struct ib_mac * ) netdev->ll_addr );
	mac->qpn = htonl ( ib_get_qpn ( mlx->ipoib_qph ) );
	memcpy ( &mac->gid, ib_data.port_gid.raw, sizeof ( mac->gid ) );

	/* Register network device */
	if ( ( rc = register_netdev ( netdev ) ) != 0 )
		goto err_register_netdev;

	return 0;

 err_register_netdev:
 err_ipoib_init:
	ib_driver_close ( 0 );
	netdev_nullify ( netdev );
	netdev_put ( netdev );
	return rc;
}

static struct pci_device_id arbel_nics[] = {
	PCI_ROM ( 0x15b3, 0x6282, "MT25218", "MT25218 HCA driver" ),
	PCI_ROM ( 0x15b3, 0x6274, "MT25204", "MT25204 HCA driver" ),
};

struct pci_driver arbel_driver __pci_driver = {
	.ids = arbel_nics,
	.id_count = ( sizeof ( arbel_nics ) / sizeof ( arbel_nics[0] ) ),
	.probe = arbel_probe,
	.remove = arbel_remove,
};
