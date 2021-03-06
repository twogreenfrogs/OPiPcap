/*
 *************************************************************************
 * Ralink Tech Inc.
 * 5F., No.36, Taiyuan St., Jhubei City,
 * Hsinchu County 302,
 * Taiwan, R.O.C.
 *
 * (c) Copyright 2002-2007, Ralink Technology, Inc.
 *
 * This program is free software; you can redistribute it and/or modify  * 
 * it under the terms of the GNU General Public License as published by  * 
 * the Free Software Foundation; either version 2 of the License, or     * 
 * (at your option) any later version.                                   * 
 *                                                                       * 
 * This program is distributed in the hope that it will be useful,       * 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        * 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         * 
 * GNU General Public License for more details.                          * 
 *                                                                       * 
 * You should have received a copy of the GNU General Public License     * 
 * along with this program; if not, write to the                         * 
 * Free Software Foundation, Inc.,                                       * 
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             * 
 *                                                                       * 
 *************************************************************************

	Module Name:
	rtusb_bulk.c

	Abstract:

	Revision History:
	Who			When		What
	--------	----------	----------------------------------------------
	Name		Date		Modification logs
	Paul Lin	06-25-2004	created
	
*/

#ifdef RTMP_MAC_USB


#include	"rt_config.h"
/* Match total 6 bulkout endpoint to corresponding queue.*/
UCHAR	EpToQueue[6]={FIFO_EDCA, FIFO_EDCA, FIFO_EDCA, FIFO_EDCA, FIFO_EDCA, FIFO_MGMT};

/*static BOOLEAN SingleBulkOut = FALSE;*/


VOID	RTUSBInitTxDesc(
	IN	PRTMP_ADAPTER	pAd,
	IN	PTX_CONTEXT		pTxContext,
	IN	UCHAR			BulkOutPipeId,
	IN	usb_complete_t	Func)
{
	PURB				pUrb;
	PUCHAR				pSrc = NULL;
	POS_COOKIE			pObj = (POS_COOKIE) pAd->OS_Cookie;

	pUrb = pTxContext->pUrb;
	ASSERT(pUrb);

	/* Store BulkOut PipeId*/
	pTxContext->BulkOutPipeId = BulkOutPipeId;

	if (pTxContext->bAggregatible)
	{
		pSrc = &pTxContext->TransferBuffer->Aggregation[2];

		/*Initialize a tx bulk urb*/
		RTUSB_FILL_TX_BULK_URB(pUrb,
						pObj->pUsb_Dev,
						pAd->BulkOutEpAddr[BulkOutPipeId],
						pSrc,
						pTxContext->BulkOutSize,
						Func,
						pTxContext,
						(pTxContext->data_dma + TX_BUFFER_NORMSIZE + 2));
	}
	else
	{
		pSrc = (PUCHAR) pTxContext->TransferBuffer->field.WirelessPacket;

		/*Initialize a tx bulk urb*/
		RTUSB_FILL_TX_BULK_URB(pUrb,
						pObj->pUsb_Dev,
						pAd->BulkOutEpAddr[BulkOutPipeId],
						pSrc,
						pTxContext->BulkOutSize,
						Func,
						pTxContext,
						pTxContext->data_dma);
	}
}

VOID	RTUSBInitHTTxDesc(
	IN	PRTMP_ADAPTER	pAd,
	IN	PHT_TX_CONTEXT	pTxContext,
	IN	UCHAR			BulkOutPipeId,
	IN	ULONG			BulkOutSize,
	IN	usb_complete_t	Func)
{
	PURB				pUrb;
	PUCHAR				pSrc = NULL;
	POS_COOKIE			pObj = (POS_COOKIE) pAd->OS_Cookie;

	pUrb = pTxContext->pUrb;
	ASSERT(pUrb);

	/* Store BulkOut PipeId*/
	pTxContext->BulkOutPipeId = BulkOutPipeId;

	pSrc = &pTxContext->TransferBuffer->field.WirelessPacket[pTxContext->NextBulkOutPosition];

	/*Initialize a tx bulk urb*/
	RTUSB_FILL_HTTX_BULK_URB(pUrb,
						pObj->pUsb_Dev,
						pAd->BulkOutEpAddr[BulkOutPipeId],
						pSrc,
						BulkOutSize,
						Func,
						pTxContext,
						(pTxContext->data_dma + pTxContext->NextBulkOutPosition));
}

VOID	RTUSBInitRxDesc(
	IN	PRTMP_ADAPTER	pAd,
	IN	PRX_CONTEXT		pRxContext)
{
	PURB				pUrb;
	POS_COOKIE			pObj = (POS_COOKIE) pAd->OS_Cookie;
	ULONG				RX_bulk_size;


	pUrb = pRxContext->pUrb;
	ASSERT(pUrb);
	
	if ( pAd->BulkInMaxPacketSize == 64)
		RX_bulk_size = 4096;
	else
		RX_bulk_size = MAX_RXBULK_SIZE;
		

	/*Initialize a rx bulk urb*/
	RTUSB_FILL_RX_BULK_URB(pUrb,
					pObj->pUsb_Dev,
					pAd->BulkInEpAddr,
					&(pRxContext->TransferBuffer[pAd->NextRxBulkInPosition]),
					RX_bulk_size - (pAd->NextRxBulkInPosition),
					RtmpUsbBulkRxComplete,
					(void *)pRxContext,
					(pRxContext->data_dma + pAd->NextRxBulkInPosition));
}

/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note:
	
	========================================================================
*/

#define BULK_OUT_LOCK(pLock, IrqFlags)	\
		if(1 /*!(in_interrupt() & 0xffff0000)*/)	\
			RTMP_IRQ_LOCK((pLock), IrqFlags);
		
#define BULK_OUT_UNLOCK(pLock, IrqFlags)	\
		if(1 /*!(in_interrupt() & 0xffff0000)*/)	\
			RTMP_IRQ_UNLOCK((pLock), IrqFlags);


VOID	RTUSBBulkOutDataPacket(
	IN	PRTMP_ADAPTER	pAd,
	IN	UCHAR			BulkOutPipeId,
	IN	UCHAR			Index)
{

	PHT_TX_CONTEXT	pHTTXContext;
	PURB			pUrb;
	int				ret = 0;
	PTXINFO_STRUC	pTxInfo, pLastTxInfo = NULL;
	PTXWI_STRUC             pTxWI;
	ULONG			TmpBulkEndPos, ThisBulkSize;
	unsigned long	IrqFlags = 0, IrqFlags2 = 0;
	PUCHAR			pWirelessPkt, pAppendant;

#ifdef USB_BULK_BUF_ALIGMENT
	BOOLEAN			bLasAlignmentsectiontRound = FALSE;
#else
	BOOLEAN			bTxQLastRound = FALSE;
	UCHAR			allzero[4]= {0x0,0x0,0x0,0x0};
#endif /* USB_BULK_BUF_ALIGMENT */



	BULK_OUT_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
	if ((pAd->BulkOutPending[BulkOutPipeId] == TRUE) || RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NEED_STOP_TX))
	{
		BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
		return;
	}
	pAd->BulkOutPending[BulkOutPipeId] = TRUE;
	
	if (!OPSTATUS_TEST_FLAG(pAd, fOP_STATUS_MEDIA_STATE_CONNECTED)
		)
	{
		pAd->BulkOutPending[BulkOutPipeId] = FALSE;
		BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
		return;
	}
	BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);


	pHTTXContext = &(pAd->TxContext[BulkOutPipeId]);

	BULK_OUT_LOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags2);
	if ((pHTTXContext->ENextBulkOutPosition == pHTTXContext->CurWritePosition)
#ifdef USB_BULK_BUF_ALIGMENT
		|| ((pHTTXContext->CurWriteRealPos > pHTTXContext->CurWritePosition) &&(pHTTXContext->NextBulkIdx == pHTTXContext->CurWriteIdx) )
#else
		|| ((pHTTXContext->ENextBulkOutPosition-8) == pHTTXContext->CurWritePosition)
#endif /* USB_BULK_BUF_ALIGMENT */
		)  /* druing writing. */
	{
		BULK_OUT_UNLOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags2);
		
		BULK_OUT_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
		pAd->BulkOutPending[BulkOutPipeId] = FALSE;
		
		/* Clear Data flag*/
		RTUSB_CLEAR_BULK_FLAG(pAd, (fRTUSB_BULK_OUT_DATA_FRAG << BulkOutPipeId));
		RTUSB_CLEAR_BULK_FLAG(pAd, (fRTUSB_BULK_OUT_DATA_NORMAL << BulkOutPipeId));
		
		BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
		return;
	}

	/* Clear Data flag*/
	RTUSB_CLEAR_BULK_FLAG(pAd, (fRTUSB_BULK_OUT_DATA_FRAG << BulkOutPipeId));
	RTUSB_CLEAR_BULK_FLAG(pAd, (fRTUSB_BULK_OUT_DATA_NORMAL << BulkOutPipeId));

	/*DBGPRINT(RT_DEBUG_TRACE,("BulkOut-B:I=0x%lx, CWPos=%ld, CWRPos=%ld, NBPos=%ld, ENBPos=%ld, bCopy=%d!\n", in_interrupt(), */
	/*							pHTTXContext->CurWritePosition, pHTTXContext->CurWriteRealPos, pHTTXContext->NextBulkOutPosition, */
	/*							pHTTXContext->ENextBulkOutPosition, pHTTXContext->bCopySavePad));*/
	pHTTXContext->NextBulkOutPosition = pHTTXContext->ENextBulkOutPosition;	
	ThisBulkSize = 0;
	TmpBulkEndPos = pHTTXContext->NextBulkOutPosition;
	pWirelessPkt = &pHTTXContext->TransferBuffer->field.WirelessPacket[0];

#ifndef USB_BULK_BUF_ALIGMENT	
	if ((pHTTXContext->bCopySavePad == TRUE))
	{
		if (RTMPEqualMemory(pHTTXContext->SavedPad, allzero,4))
		{
			DBGPRINT_RAW(RT_DEBUG_ERROR,("e1, allzero : %x  %x  %x  %x  %x  %x  %x  %x \n",
				pHTTXContext->SavedPad[0], pHTTXContext->SavedPad[1], pHTTXContext->SavedPad[2],pHTTXContext->SavedPad[3]
				,pHTTXContext->SavedPad[4], pHTTXContext->SavedPad[5], pHTTXContext->SavedPad[6],pHTTXContext->SavedPad[7]));
		}
		NdisMoveMemory(&pWirelessPkt[TmpBulkEndPos], pHTTXContext->SavedPad, 8);
		pHTTXContext->bCopySavePad = FALSE;
		if (pAd->bForcePrintTX == TRUE)
			DBGPRINT(RT_DEBUG_TRACE,("RTUSBBulkOutDataPacket --> COPY PAD. CurWrite = %ld, NextBulk = %ld.   ENextBulk = %ld.\n",   pHTTXContext->CurWritePosition, pHTTXContext->NextBulkOutPosition, pHTTXContext->ENextBulkOutPosition));
	}
#endif /* USB_BULK_BUF_ALIGMENT */


	do
	{
		pTxInfo = (PTXINFO_STRUC)&pWirelessPkt[TmpBulkEndPos];
		pTxWI = (PTXWI_STRUC)&pWirelessPkt[TmpBulkEndPos + TXINFO_SIZE];
	
		if (pAd->bForcePrintTX == TRUE)
			DBGPRINT(RT_DEBUG_TRACE, ("RTUSBBulkOutDataPacket AMPDU = %d.\n",   pTxWI->AMPDU));
		
		/* add by Iverson, limit BulkOut size to 4k to pass WMM b mode 2T1R test items*/
		/*if ((ThisBulkSize != 0)  && (pTxWI->AMPDU == 0))*/
		if ((ThisBulkSize != 0) && (pTxWI->PHYMODE == MODE_CCK))
		{
#ifdef INF_AMAZON_SE
			/*Iverson Add for AMAZON USB (RT2070 &&  RT3070) to pass WMM A2-T4 ~ A2-T10*/
			if(OPSTATUS_TEST_FLAG(pAd, fOP_STATUS_WMM_INUSED))
			{
				/*Iverson patch for WMM A5-T07 ,WirelessStaToWirelessSta do not bulk out aggregate*/
				if(pTxWI->PacketId == 6)
                {            
                	pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
                	break;
                }    
				else if (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&pAd->BulkOutDataSizeLimit[BulkOutPipeId]) == pAd->BulkOutDataSizeLimit[BulkOutPipeId]))
				{	
			/*printk("===Bulkout size limit :%d ===\n",MaxBulkOutSize);*/
			/*DBGPRINT(RT_DEBUG_TRACE,("b mode BulkOutPipeId %d  pAd->BulkOutDataSizeLimit[BulkOutPipeId] %d  \n",BulkOutPipeId,pAd->BulkOutDataSizeLimit[BulkOutPipeId]));*/
					pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
					break;
				}
				
			}
			else if (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&0x1000) == 0x1000))
			{
				/* Limit BulkOut size to about 4k bytes.*/
				pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
				break;
			}
#endif /* INF_AMAZON_SE */
#ifndef INF_AMAZON_SE
#ifndef USB_BULK_BUF_ALIGMENT
			if (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&0x1000) == 0x1000))
			{
				/* Limit BulkOut size to about 4k bytes.*/
				pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
				break;
			}
#else
			if (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&0x6000) == 0x6000))
			{
				/* Limit BulkOut size to about 24k bytes.*/
				pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;

				/*
					when bulk size is > 6000, it mean that this is the lasttround at this alignmnet section.
				*/
				bLasAlignmentsectiontRound = TRUE;				
				break;
			}

#endif /* USB_BULK_BUF_ALIGMENT */
#endif /* INF_AMAZON_SE */
#ifndef USB_BULK_BUF_ALIGMENT
			else if (((pAd->BulkOutMaxPacketSize < 512) && ((ThisBulkSize&0xfffff800) != 0) ) /*|| ( (ThisBulkSize != 0)  && (pTxWI->AMPDU == 0))*/)
			{
				/* For USB 1.1 or peer which didn't support AMPDU, limit the BulkOut size. */
				/* For performence in b/g mode, now just check for USB 1.1 and didn't care about the APMDU or not! 2008/06/04.*/
				pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
				break;
			}
#else
			else if (((pAd->BulkOutMaxPacketSize < 512) && (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&0x6000) == 0x6000)) ))
			{
				/* Limit BulkOut size to about 24k bytes.*/
				pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;

				/*
					when bulk size is > 6000, it mean that this is the lasttround at this alignmnet section.
				*/
				bLasAlignmentsectiontRound = TRUE;				
				break;
			}

#endif /* USB_BULK_BUF_ALIGMENT */

		}
		/* end Iverson*/
		else
		{


		if (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&0x6000) == 0x6000))
		{	/* Limit BulkOut size to about 24k bytes.*/
			pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
#ifdef USB_BULK_BUF_ALIGMENT
				/*
					when bulk size is > 0x6000, it mean that this is the lasttround at this alignmnet section.
				*/
				bLasAlignmentsectiontRound = TRUE;
/*				printk("data bulk out bLasAlignmentsectiontRound \n");*/
#endif /* USB_BULK_BUF_ALIGMENT */

			break;
		}
#ifdef INF_AMAZON_SE
		else if (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&pAd->BulkOutDataSizeLimit[BulkOutPipeId]) == pAd->BulkOutDataSizeLimit[BulkOutPipeId]))
		{	
			/*printk("===Bulkout size limit :%d ===\n",ThisBulkSize);*/
			pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
			break;
		}
#endif /* INF_AMAZON_SE */
#ifndef USB_BULK_BUF_ALIGMENT
		else if (((pAd->BulkOutMaxPacketSize < 512) && ((ThisBulkSize&0xfffff800) != 0) ) /*|| ( (ThisBulkSize != 0)  && (pTxWI->AMPDU == 0))*/)
		{	/* For USB 1.1 or peer which didn't support AMPDU, limit the BulkOut size. */
			/* For performence in b/g mode, now just check for USB 1.1 and didn't care about the APMDU or not! 2008/06/04.*/
			pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
			break;
		}
#else
			else if (((pAd->BulkOutMaxPacketSize < 512) && (((ThisBulkSize&0xffff8000) != 0) || ((ThisBulkSize&0x6000) == 0x6000)) ))
			{
				/* Limit BulkOut size to about 24k bytes.*/
				pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;

				/*
					when bulk size is > 6000, it mean that this is the lasttround at this alignmnet section.
				*/
				bLasAlignmentsectiontRound = TRUE;				
				break;
			}

#endif /* USB_BULK_BUF_ALIGMENT */



		}
		
		if (TmpBulkEndPos == pHTTXContext->CurWritePosition)
		{
			pHTTXContext->ENextBulkOutPosition = TmpBulkEndPos;
			break;
		}
		
		if (pTxInfo->QSEL != FIFO_EDCA)
		{
			DBGPRINT(RT_DEBUG_ERROR, ("%s(): ====> pTxInfo->QueueSel(%d)!= FIFO_EDCA!!!!\n", 
										__FUNCTION__, pTxInfo->QSEL));
			DBGPRINT(RT_DEBUG_ERROR, ("\tCWPos=%ld, NBPos=%ld, ENBPos=%ld, bCopy=%d!\n", 
										pHTTXContext->CurWritePosition, pHTTXContext->NextBulkOutPosition, 
										pHTTXContext->ENextBulkOutPosition, pHTTXContext->bCopySavePad));
			hex_dump("Wrong QSel Pkt:", (PUCHAR)&pWirelessPkt[TmpBulkEndPos], (pHTTXContext->CurWritePosition - pHTTXContext->NextBulkOutPosition));
		}
		
		if (pTxInfo->USBDMATxPktLen <= 8)
		{
			BULK_OUT_UNLOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags2);
			DBGPRINT(RT_DEBUG_ERROR /*RT_DEBUG_TRACE*/,("e2, USBDMATxPktLen==0, Size=%ld, bCSPad=%d, CWPos=%ld, NBPos=%ld, CWRPos=%ld!\n", 
					pHTTXContext->BulkOutSize, pHTTXContext->bCopySavePad, pHTTXContext->CurWritePosition, pHTTXContext->NextBulkOutPosition, pHTTXContext->CurWriteRealPos));
			{
				DBGPRINT_RAW(RT_DEBUG_ERROR /*RT_DEBUG_TRACE*/,("%x  %x  %x  %x  %x  %x  %x  %x \n",
					pHTTXContext->SavedPad[0], pHTTXContext->SavedPad[1], pHTTXContext->SavedPad[2],pHTTXContext->SavedPad[3]
					,pHTTXContext->SavedPad[4], pHTTXContext->SavedPad[5], pHTTXContext->SavedPad[6],pHTTXContext->SavedPad[7]));
			}
			pAd->bForcePrintTX = TRUE;
			BULK_OUT_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
			pAd->BulkOutPending[BulkOutPipeId] = FALSE;
			BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
			/*DBGPRINT(RT_DEBUG_LOUD,("Out:pTxInfo->USBDMATxPktLen=%d!\n", pTxInfo->USBDMATxPktLen));*/
			return;
		}

			/* Increase Total transmit byte counter*/
		pAd->RalinkCounters.OneSecTransmittedByteCount +=  pTxWI->MPDUtotalByteCount;
		pAd->RalinkCounters.TransmittedByteCount +=  pTxWI->MPDUtotalByteCount;
	
		pLastTxInfo = pTxInfo;
		
		/* Make sure we use EDCA QUEUE.  */
		pTxInfo->QSEL = FIFO_EDCA;
		ThisBulkSize += (pTxInfo->USBDMATxPktLen+4);
		TmpBulkEndPos += (pTxInfo->USBDMATxPktLen+4);
		
		if (TmpBulkEndPos != pHTTXContext->CurWritePosition)
			pTxInfo->USBDMANextVLD = 1;

#ifdef USB_BULK_BUF_ALIGMENT		
/*
		this is for frag packet , because it will finish this section 
		when ((((pHTTXContext->CurWritePosition + 3906)& 0x00007fff) & 0xffff6000) == 0x00006000)	
*/
		if (pTxInfo->bFragLasAlignmentsectiontRound == 1)
		{
			bLasAlignmentsectiontRound = TRUE;
			break;
		}
#else		
		if (pTxInfo->SwUseLastRound == 1)
		{
			if (pHTTXContext->CurWritePosition == 8)
				pTxInfo->USBDMANextVLD = 0;
			pTxInfo->SwUseLastRound = 0;
			
			bTxQLastRound = TRUE;
			pHTTXContext->ENextBulkOutPosition = 8;
			
	#ifdef RT_BIG_ENDIAN
			RTMPDescriptorEndianChange((PUCHAR)pTxInfo, TYPE_TXINFO);
			RTMPWIEndianChange((PUCHAR)pTxWI, TYPE_TXWI);
	#endif /* RT_BIG_ENDIAN */
	
			break;
		}
#endif /* USB_BULK_BUF_ALIGMENT */	
#ifdef RT_BIG_ENDIAN
		RTMPDescriptorEndianChange((PUCHAR)pTxInfo, TYPE_TXINFO);
		RTMPWIEndianChange((PUCHAR)pTxWI, TYPE_TXWI);
#endif /* RT_BIG_ENDIAN */

	}while (TRUE);

	/* adjust the pTxInfo->USBDMANextVLD value of last pTxInfo.*/
	if (pLastTxInfo)
	{
#ifdef RT_BIG_ENDIAN
		RTMPDescriptorEndianChange((PUCHAR)pLastTxInfo, TYPE_TXINFO);
#endif /* RT_BIG_ENDIAN */
		pLastTxInfo->USBDMANextVLD = 0;
#ifdef RT_BIG_ENDIAN
		RTMPDescriptorEndianChange((PUCHAR)pLastTxInfo, TYPE_TXINFO);
#endif /* RT_BIG_ENDIAN */
	}

	/* 
		We need to copy SavedPad when following condition matched!
			1. Not the last round of the TxQueue and
			2. any match of following cases:
				(1). The End Position of this bulk out is reach to the Currenct Write position and 
						the TxInfo and related header already write to the CurWritePosition.
			   		=>(ENextBulkOutPosition == CurWritePosition) && (CurWriteRealPos > CurWritePosition)
		
				(2). The EndPosition of the bulk out is not reach to the Current Write Position.
					=>(ENextBulkOutPosition != CurWritePosition)
	*/
#ifndef USB_BULK_BUF_ALIGMENT
	if ((bTxQLastRound == FALSE) &&
		 (((pHTTXContext->ENextBulkOutPosition == pHTTXContext->CurWritePosition) && (pHTTXContext->CurWriteRealPos > pHTTXContext->CurWritePosition)) ||
		  (pHTTXContext->ENextBulkOutPosition != pHTTXContext->CurWritePosition))
		)
	{
		NdisMoveMemory(pHTTXContext->SavedPad, &pWirelessPkt[pHTTXContext->ENextBulkOutPosition], 8);
		pHTTXContext->bCopySavePad = TRUE;
		if (RTMPEqualMemory(pHTTXContext->SavedPad, allzero,4))
		{	
			PUCHAR	pBuf = &pHTTXContext->SavedPad[0];
			DBGPRINT_RAW(RT_DEBUG_ERROR,("WARNING-Zero-3:%02x%02x%02x%02x%02x%02x%02x%02x,CWPos=%ld, CWRPos=%ld, bCW=%d, NBPos=%ld, TBPos=%ld, TBSize=%ld\n",
				pBuf[0], pBuf[1], pBuf[2],pBuf[3],pBuf[4], pBuf[5], pBuf[6],pBuf[7], pHTTXContext->CurWritePosition, pHTTXContext->CurWriteRealPos,
				pHTTXContext->bCurWriting, pHTTXContext->NextBulkOutPosition, TmpBulkEndPos, ThisBulkSize));
			
			pBuf = &pWirelessPkt[pHTTXContext->CurWritePosition];
			DBGPRINT_RAW(RT_DEBUG_ERROR,("\tCWPos=%02x%02x%02x%02x%02x%02x%02x%02x\n", pBuf[0], pBuf[1], pBuf[2],pBuf[3],pBuf[4], pBuf[5], pBuf[6],pBuf[7]));
		}
		/*DBGPRINT(RT_DEBUG_LOUD,("ENPos==CWPos=%ld, CWRPos=%ld, bCSPad=%d!\n", pHTTXContext->CurWritePosition, pHTTXContext->CurWriteRealPos, pHTTXContext->bCopySavePad));*/
	}
#endif /* USB_BULK_BUF_ALIGMENT */

	if (pAd->bForcePrintTX == TRUE)
		DBGPRINT(RT_DEBUG_TRACE,("BulkOut-A:Size=%ld, CWPos=%ld, NBPos=%ld, ENBPos=%ld, bCopy=%d!\n", ThisBulkSize, pHTTXContext->CurWritePosition, pHTTXContext->NextBulkOutPosition, pHTTXContext->ENextBulkOutPosition, pHTTXContext->bCopySavePad));
	/*DBGPRINT(RT_DEBUG_LOUD,("BulkOut-A:Size=%ld, CWPos=%ld, CWRPos=%ld, NBPos=%ld, ENBPos=%ld, bCopy=%d, bLRound=%d!\n", ThisBulkSize, pHTTXContext->CurWritePosition, pHTTXContext->CurWriteRealPos, pHTTXContext->NextBulkOutPosition, pHTTXContext->ENextBulkOutPosition, pHTTXContext->bCopySavePad, bTxQLastRound));*/
	
		/* USB DMA engine requires to pad extra 4 bytes. This pad doesn't count into real bulkoutsize.*/
	pAppendant = &pWirelessPkt[TmpBulkEndPos];
	NdisZeroMemory(pAppendant, 8);
		ThisBulkSize += 4;
		pHTTXContext->LastOne = TRUE;

	pHTTXContext->BulkOutSize = ThisBulkSize;
#ifdef USB_BULK_BUF_ALIGMENT
	/*
		if it is the last alignment section round,that we just need to add nextbulkindex, 
		otherwise we both need to add  nextbulkindex and CurWriteIdx
		(because when alignment section round happened, the CurWriteIdx is added at function writing resource.)
	*/	
	if(bLasAlignmentsectiontRound == TRUE)
	{
			pHTTXContext->ENextBulkOutPosition = ((CUR_WRITE_IDX_INC(pHTTXContext->NextBulkIdx, BUF_ALIGMENT_RINGSIZE)) * 0x8000);
	}
	else
	{	
			pHTTXContext->ENextBulkOutPosition = ((CUR_WRITE_IDX_INC(pHTTXContext->NextBulkIdx, BUF_ALIGMENT_RINGSIZE)) * 0x8000);
			pHTTXContext->CurWritePosition = ((CUR_WRITE_IDX_INC(pHTTXContext->CurWriteIdx, BUF_ALIGMENT_RINGSIZE)) * 0x8000);
	 }

#endif /* USB_BULK_BUF_ALIGMENT */

	
	pAd->watchDogTxPendingCnt[BulkOutPipeId] = 1;
	BULK_OUT_UNLOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags2);
	
	/* Init Tx context descriptor*/
	RTUSBInitHTTxDesc(pAd, pHTTXContext, BulkOutPipeId, ThisBulkSize, (usb_complete_t)RtmpUsbBulkOutDataPacketComplete);
	
	pUrb = pHTTXContext->pUrb;
	if((ret = RTUSB_SUBMIT_URB(pUrb))!=0)
	{
		DBGPRINT(RT_DEBUG_ERROR, ("RTUSBBulkOutDataPacket: Submit Tx URB failed %d\n", ret));
		
		BULK_OUT_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
		pAd->BulkOutPending[BulkOutPipeId] = FALSE;
		pAd->watchDogTxPendingCnt[BulkOutPipeId] = 0;
		BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);

		return;
	}

	BULK_OUT_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
	pHTTXContext->IRPPending = TRUE;
	BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
	pAd->BulkOutReq++;

}


USBHST_STATUS RTUSBBulkOutDataPacketComplete(URBCompleteStatus Status, purbb_t pURB, pregs *pt_regs)
{
	PHT_TX_CONTEXT	pHTTXContext;
	PRTMP_ADAPTER	pAd;
	POS_COOKIE 		pObj;
	UCHAR			BulkOutPipeId;
	

	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_OS_USB_CONTEXT_GET(pURB);
	pAd 			= pHTTXContext->pAd;
	pObj 			= (POS_COOKIE) pAd->OS_Cookie;

	/* Store BulkOut PipeId*/
	BulkOutPipeId	= pHTTXContext->BulkOutPipeId;
	pAd->BulkOutDataOneSecCount++;

	switch (BulkOutPipeId)
	{
		case EDCA_AC0_PIPE:
#ifdef RALINK_ATE
				if (!ATE_ON(pAd))
				{
#endif /* RALINK_ATE */
					RTMP_NET_TASK_DATA_ASSIGN(&pObj->ac0_dma_done_task, (unsigned long)pURB);
					RTMP_OS_TASKLET_SCHE(&pObj->ac0_dma_done_task);
#ifdef RALINK_ATE
				}
				else
				{
					RTMP_NET_TASK_DATA_ASSIGN(&pObj->ate_ac0_dma_done_task, (unsigned long)pURB);
					RTMP_OS_TASKLET_SCHE(&pObj->ate_ac0_dma_done_task);
				}
#endif /* RALINK_ATE */
				
				break;
		case EDCA_AC1_PIPE:
				RTMP_NET_TASK_DATA_ASSIGN(&pObj->ac1_dma_done_task, (unsigned long)pURB);
				RTMP_OS_TASKLET_SCHE(&pObj->ac1_dma_done_task);
				break;
		case EDCA_AC2_PIPE:
				RTMP_NET_TASK_DATA_ASSIGN(&pObj->ac2_dma_done_task, (unsigned long)pURB);
				RTMP_OS_TASKLET_SCHE(&pObj->ac2_dma_done_task);
				break;
		case EDCA_AC3_PIPE:
				RTMP_NET_TASK_DATA_ASSIGN(&pObj->ac3_dma_done_task, (unsigned long)pURB);
				RTMP_OS_TASKLET_SCHE(&pObj->ac3_dma_done_task);
				break;
		case HCCA_PIPE:
				RTMP_NET_TASK_DATA_ASSIGN(&pObj->hcca_dma_done_task, (unsigned long)pURB);
				RTMP_OS_TASKLET_SCHE(&pObj->hcca_dma_done_task);
				break;
	}

	
}


/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note: NULL frame use BulkOutPipeId = 0
	
	========================================================================
*/
VOID	RTUSBBulkOutNullFrame(
	IN	PRTMP_ADAPTER	pAd)
{
	PTX_CONTEXT		pNullContext = &(pAd->NullContext);
	PURB			pUrb;
	int				ret = 0;
	unsigned long	IrqFlags;
	
	RTMP_IRQ_LOCK(&pAd->BulkOutLock[0], IrqFlags);
	if ((pAd->BulkOutPending[0] == TRUE) || RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NEED_STOP_TX))
	{
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], IrqFlags);
		return;
	}
	pAd->BulkOutPending[0] = TRUE;
	pAd->watchDogTxPendingCnt[0] = 1;
	pNullContext->IRPPending = TRUE;
	RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], IrqFlags);

	/* Increase Total transmit byte counter*/
	pAd->RalinkCounters.TransmittedByteCount +=  pNullContext->BulkOutSize;

	
	/* Clear Null frame bulk flag*/
	RTUSB_CLEAR_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NULL);

#ifdef RT_BIG_ENDIAN
	RTMPDescriptorEndianChange((PUCHAR)pNullContext->TransferBuffer, TYPE_TXINFO);
#endif /* RT_BIG_ENDIAN */

	/* Init Tx context descriptor*/
	RTUSBInitTxDesc(pAd, pNullContext, 0, (usb_complete_t)RtmpUsbBulkOutNullFrameComplete);

	pUrb = pNullContext->pUrb;
	if((ret = RTUSB_SUBMIT_URB(pUrb))!=0)
	{
		RTMP_IRQ_LOCK(&pAd->BulkOutLock[0], IrqFlags);
		pAd->BulkOutPending[0] = FALSE;
		pAd->watchDogTxPendingCnt[0] = 0;
		pNullContext->IRPPending = FALSE;
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], IrqFlags);
		
		DBGPRINT(RT_DEBUG_ERROR, ("RTUSBBulkOutNullFrame: Submit Tx URB failed %d\n", ret));
		return;
	}	
	
}

/* NULL frame use BulkOutPipeId = 0*/
USBHST_STATUS RTUSBBulkOutNullFrameComplete(URBCompleteStatus Status, purbb_t pURB, pregs *pt_regs)
{
	PRTMP_ADAPTER		pAd;
	PTX_CONTEXT			pNullContext;
	NTSTATUS			Status;
	POS_COOKIE			pObj;

	
	pNullContext	= (PTX_CONTEXT)RTMP_OS_USB_CONTEXT_GET(pURB);
	pAd 			= pNullContext->pAd;
	Status 			= RTMP_OS_USB_STATUS_GET(pURB); /*->rtusb_urb_status;*/

	pObj = (POS_COOKIE) pAd->OS_Cookie;
	RTMP_NET_TASK_DATA_ASSIGN(&pObj->null_frame_complete_task, (unsigned long)pURB);
	RTMP_OS_TASKLET_SCHE(&pObj->null_frame_complete_task);

}


/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note: MLME use BulkOutPipeId = 0
	
	========================================================================
*/
VOID	RTUSBBulkOutMLMEPacket(
	IN	PRTMP_ADAPTER	pAd,
	IN	UCHAR			Index)
{
	PTX_CONTEXT		pMLMEContext;
	PURB			pUrb;
	int				ret = 0;
	unsigned long	IrqFlags;
		
	pMLMEContext = (PTX_CONTEXT)pAd->MgmtRing.Cell[pAd->MgmtRing.TxDmaIdx].AllocVa;
	pUrb = pMLMEContext->pUrb;

	if ((pAd->MgmtRing.TxSwFreeIdx >= MGMT_RING_SIZE) ||
		(pMLMEContext->InUse == FALSE) ||
		(pMLMEContext->bWaitingBulkOut == FALSE))
	{
		
		
		/* Clear MLME bulk flag*/
		RTUSB_CLEAR_BULK_FLAG(pAd, fRTUSB_BULK_OUT_MLME);
		
		return;
	}


	RTMP_IRQ_LOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);
	if ((pAd->BulkOutPending[MGMTPIPEIDX] == TRUE) || RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NEED_STOP_TX))
	{
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);
		return;
	}

	pAd->BulkOutPending[MGMTPIPEIDX] = TRUE;
	pAd->watchDogTxPendingCnt[MGMTPIPEIDX] = 1;
	pMLMEContext->IRPPending = TRUE;
	pMLMEContext->bWaitingBulkOut = FALSE;
	RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);
	
	/* Increase Total transmit byte counter*/
	pAd->RalinkCounters.TransmittedByteCount +=  pMLMEContext->BulkOutSize;

	/* Clear MLME bulk flag*/
	RTUSB_CLEAR_BULK_FLAG(pAd, fRTUSB_BULK_OUT_MLME);

#ifdef RT_BIG_ENDIAN
	RTMPDescriptorEndianChange((PUCHAR)pMLMEContext->TransferBuffer, TYPE_TXINFO);
#endif /* RT_BIG_ENDIAN */

	/* Init Tx context descriptor*/
	RTUSBInitTxDesc(pAd, pMLMEContext, MGMTPIPEIDX, (usb_complete_t)RtmpUsbBulkOutMLMEPacketComplete);

	RTUSB_URB_DMA_MAPPING(pUrb);

	pUrb = pMLMEContext->pUrb;
	if((ret = RTUSB_SUBMIT_URB(pUrb))!=0)
	{
		DBGPRINT(RT_DEBUG_ERROR, ("RTUSBBulkOutMLMEPacket: Submit MLME URB failed %d\n", ret));
		RTMP_IRQ_LOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);
		pAd->BulkOutPending[MGMTPIPEIDX] = FALSE;
		pAd->watchDogTxPendingCnt[MGMTPIPEIDX] = 0;
		pMLMEContext->IRPPending = FALSE;
		pMLMEContext->bWaitingBulkOut = TRUE;
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);
		
		return;
	}
}


USBHST_STATUS RTUSBBulkOutMLMEPacketComplete(URBCompleteStatus Status, purbb_t pURB, pregs *pt_regs)
{
	PTX_CONTEXT			pMLMEContext;
	PRTMP_ADAPTER		pAd;
	NTSTATUS			Status;
	POS_COOKIE 			pObj;
	int					index;
	
	pMLMEContext	= (PTX_CONTEXT)RTMP_OS_USB_CONTEXT_GET(pURB); 
	pAd 			= pMLMEContext->pAd;
	pObj 			= (POS_COOKIE)pAd->OS_Cookie;
	Status			= RTMP_OS_USB_STATUS_GET(pURB);
	index 			= pMLMEContext->SelfIdx;

	RTMP_NET_TASK_DATA_ASSIGN(&pObj->mgmt_dma_done_task, (unsigned long)pURB);
	RTMP_OS_TASKLET_SCHE(&pObj->mgmt_dma_done_task);
}


/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note: PsPoll use BulkOutPipeId = 0
	
	========================================================================
*/
VOID	RTUSBBulkOutPsPoll(
	IN	PRTMP_ADAPTER	pAd)
{
	PTX_CONTEXT		pPsPollContext = &(pAd->PsPollContext);
	PURB			pUrb;
	int				ret = 0;
	unsigned long	IrqFlags;
	
	RTMP_IRQ_LOCK(&pAd->BulkOutLock[0], IrqFlags);
	if ((pAd->BulkOutPending[0] == TRUE) || RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NEED_STOP_TX))
	{
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], IrqFlags);
		return;
	}
	pAd->BulkOutPending[0] = TRUE;
	pAd->watchDogTxPendingCnt[0] = 1;
	pPsPollContext->IRPPending = TRUE;
	RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], IrqFlags);

	
	/* Clear PS-Poll bulk flag*/
	RTUSB_CLEAR_BULK_FLAG(pAd, fRTUSB_BULK_OUT_PSPOLL);

#ifdef RT_BIG_ENDIAN
	RTMPDescriptorEndianChange((PUCHAR)pPsPollContext->TransferBuffer, TYPE_TXINFO);
#endif /* RT_BIG_ENDIAN */

	/* Init Tx context descriptor*/
	RTUSBInitTxDesc(pAd, pPsPollContext, MGMTPIPEIDX, (usb_complete_t)RtmpUsbBulkOutPsPollComplete);
	
	pUrb = pPsPollContext->pUrb;
	if((ret = RTUSB_SUBMIT_URB(pUrb))!=0)
	{
		RTMP_IRQ_LOCK(&pAd->BulkOutLock[0], IrqFlags);
		pAd->BulkOutPending[0] = FALSE;
		pAd->watchDogTxPendingCnt[0] = 0;
		pPsPollContext->IRPPending = FALSE;
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], IrqFlags);
		
		DBGPRINT(RT_DEBUG_ERROR, ("RTUSBBulkOutPsPoll: Submit Tx URB failed %d\n", ret));
		return;
	}
		
}

/* PS-Poll frame use BulkOutPipeId = 0*/
USBHST_STATUS RTUSBBulkOutPsPollComplete(URBCompleteStatus Status, purbb_t pURB, pregs *pt_regs)
{
	PRTMP_ADAPTER		pAd;
	PTX_CONTEXT			pPsPollContext;
	NTSTATUS			Status;
	POS_COOKIE			pObj;
	
	
	pPsPollContext= (PTX_CONTEXT)RTMP_OS_USB_CONTEXT_GET(pURB);
	pAd = pPsPollContext->pAd;
	Status = RTMP_OS_USB_STATUS_GET(pURB);

	pObj = (POS_COOKIE) pAd->OS_Cookie;
	RTMP_NET_TASK_DATA_ASSIGN(&pObj->pspoll_frame_complete_task, (unsigned long)pURB);
	RTMP_OS_TASKLET_SCHE(&pObj->pspoll_frame_complete_task);

}


VOID DoBulkIn(IN RTMP_ADAPTER *pAd)
{	
	PRX_CONTEXT		pRxContext;
	PURB			pUrb;
	int				ret = 0;
	unsigned long	IrqFlags;
	
	RTMP_IRQ_LOCK(&pAd->BulkInLock, IrqFlags);
	pRxContext = &(pAd->RxContext[pAd->NextRxBulkInIndex]);
	if ((pAd->PendingRx > 0) || (pRxContext->Readable == TRUE) || (pRxContext->InUse == TRUE))
	{
		RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);
		return;
	}
	pRxContext->InUse = TRUE;
	pRxContext->IRPPending = TRUE;
	pAd->PendingRx++;
	pAd->BulkInReq++;
	RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);

	/* Init Rx context descriptor*/
	NdisZeroMemory(pRxContext->TransferBuffer, pRxContext->BulkInOffset);
	RTUSBInitRxDesc(pAd, pRxContext);

	pUrb = pRxContext->pUrb;
	if ((ret = RTUSB_SUBMIT_URB(pUrb))!=0)
	{	/* fail*/
	
		RTMP_IRQ_LOCK(&pAd->BulkInLock, IrqFlags);
		pRxContext->InUse = FALSE;
		pRxContext->IRPPending = FALSE;
		pAd->PendingRx--;
		pAd->BulkInReq--;
		RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);
		DBGPRINT(RT_DEBUG_ERROR, ("RTUSBBulkReceive: Submit Rx URB failed %d\n", ret));
	}
	else
	{	/* success*/
		ASSERT((pRxContext->InUse == pRxContext->IRPPending));
	}
}


/*
	========================================================================

	Routine Description:
	USB_RxPacket initializes a URB and uses the Rx IRP to submit it
	to USB. It checks if an Rx Descriptor is available and passes the
	the coresponding buffer to be filled. If no descriptor is available
	fails the request. When setting the completion routine we pass our
	Adapter Object as Context.
		
	Arguments:
		
	Return Value:
		TRUE			found matched tuple cache
		FALSE			no matched found

	Note:
	
	========================================================================
*/
#define fRTMP_ADAPTER_NEED_STOP_RX		\
		(fRTMP_ADAPTER_NIC_NOT_EXIST | fRTMP_ADAPTER_HALT_IN_PROGRESS |	\
		 fRTMP_ADAPTER_RADIO_OFF | fRTMP_ADAPTER_RESET_IN_PROGRESS | \
		 fRTMP_ADAPTER_REMOVE_IN_PROGRESS | fRTMP_ADAPTER_BULKIN_RESET)
		 
#define fRTMP_ADAPTER_NEED_STOP_HANDLE_RX	\
		(fRTMP_ADAPTER_NIC_NOT_EXIST | fRTMP_ADAPTER_HALT_IN_PROGRESS |	\
		 fRTMP_ADAPTER_RADIO_OFF | fRTMP_ADAPTER_RESET_IN_PROGRESS | \
		 fRTMP_ADAPTER_REMOVE_IN_PROGRESS)
		 
VOID	RTUSBBulkReceive(
	IN	PRTMP_ADAPTER	pAd)
{
	PRX_CONTEXT		pRxContext;
	unsigned long	IrqFlags;
	

	/* sanity check */
	if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NEED_STOP_HANDLE_RX))
		return;
		
	while(1)
	{
		RTMP_IRQ_LOCK(&pAd->BulkInLock, IrqFlags);
		pRxContext = &(pAd->RxContext[pAd->NextRxBulkInReadIndex]);
		if (((pRxContext->InUse == FALSE) && (pRxContext->Readable == TRUE)) &&
			(pRxContext->bRxHandling == FALSE))
		{
			pRxContext->bRxHandling = TRUE;
			RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);

			/* read RxContext, Since not */
#ifdef CONFIG_STA_SUPPORT
			IF_DEV_CONFIG_OPMODE_ON_STA(pAd)
				STARxDoneInterruptHandle(pAd, TRUE);
#endif /* CONFIG_STA_SUPPORT */

			/* Finish to handle this bulkIn buffer.*/
			RTMP_IRQ_LOCK(&pAd->BulkInLock, IrqFlags);
			pRxContext->BulkInOffset = 0;
			pRxContext->Readable = FALSE;
			pRxContext->bRxHandling = FALSE;
			pAd->ReadPosition = 0;
			pAd->TransferBufferLength = 0;
			INC_RING_INDEX(pAd->NextRxBulkInReadIndex, RX_RING_SIZE);
			RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);
	
		} 
		else
		{
			RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);
			break;
		}
	}

	if (!(RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NEED_STOP_RX))) 
	{
#ifdef CONFIG_STA_SUPPORT
		if(RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_IDLE_RADIO_OFF))
			return;
#endif /* CONFIG_STA_SUPPORT */

		DoBulkIn(pAd);
	}
	
}


/*
	========================================================================

	Routine Description:
		This routine process Rx Irp and call rx complete function.
		
	Arguments:
		DeviceObject	Pointer to the device object for next lower
						device. DeviceObject passed in here belongs to
						the next lower driver in the stack because we
						were invoked via IoCallDriver in USB_RxPacket
						AND it is not OUR device object
	  Irp				Ptr to completed IRP
	  Context			Ptr to our Adapter object (context specified
						in IoSetCompletionRoutine
		
	Return Value:
		Always returns STATUS_MORE_PROCESSING_REQUIRED

	Note:
		Always returns STATUS_MORE_PROCESSING_REQUIRED
	========================================================================
*/
USBHST_STATUS RTUSBBulkRxComplete(URBCompleteStatus Status, purbb_t pURB, pregs *pt_regs)
{
	/* use a receive tasklet to handle received packets;*/
	/* or sometimes hardware IRQ will be disabled here, so we can not*/
	/* use spin_lock_bh()/spin_unlock_bh() after IRQ is disabled. :<*/
	PRX_CONTEXT		pRxContext;
	PRTMP_ADAPTER	pAd;
	POS_COOKIE 		pObj;

	pRxContext	= (PRX_CONTEXT)RTMP_OS_USB_CONTEXT_GET(pURB);
	pAd 		= pRxContext->pAd;
	pObj 		= (POS_COOKIE) pAd->OS_Cookie;

	RTMP_NET_TASK_DATA_ASSIGN(&pObj->rx_done_task, (unsigned long)pURB);
	RTMP_OS_TASKLET_SCHE(&pObj->rx_done_task);
	
}


/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note:
	
	========================================================================
*/
VOID	RTUSBKickBulkOut(
	IN	PRTMP_ADAPTER pAd)
{
	/* BulkIn Reset will reset whole USB PHY. So we need to make sure fRTMP_ADAPTER_BULKIN_RESET not flaged.*/
	if (!RTMP_TEST_FLAG(pAd ,fRTMP_ADAPTER_NEED_STOP_TX)
#ifdef RALINK_ATE			
		&& !(ATE_ON(pAd))
#endif /* RALINK_ATE */
		)
	{

		/* 2. PS-Poll frame is next*/
		if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_PSPOLL))
		{
			RTUSBBulkOutPsPoll(pAd);
		}

		/* 5. Mlme frame is next*/
		else if ((RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_MLME)) ||
				 (pAd->MgmtRing.TxSwFreeIdx < MGMT_RING_SIZE))
		{
			RTUSBBulkOutMLMEPacket(pAd, pAd->MgmtRing.TxDmaIdx);
		}

		/* 6. Data frame normal is next*/
		if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL))
		{
			if (((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BSS_SCAN_IN_PROGRESS)) || 
				(!OPSTATUS_TEST_FLAG(pAd, fOP_STATUS_MEDIA_STATE_CONNECTED))
				))
			{
				RTUSBBulkOutDataPacket(pAd, EDCA_AC0_PIPE, pAd->NextBulkOutIndex[EDCA_AC0_PIPE]);
			}
		}
		if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL_2))
		{		
			if (((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BSS_SCAN_IN_PROGRESS)) || 
				(!OPSTATUS_TEST_FLAG(pAd, fOP_STATUS_MEDIA_STATE_CONNECTED))
				))
			{
				RTUSBBulkOutDataPacket(pAd, EDCA_AC1_PIPE, pAd->NextBulkOutIndex[EDCA_AC1_PIPE]);
			}
		}
		if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL_3))
		{
			if (((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BSS_SCAN_IN_PROGRESS)) || 
				(!OPSTATUS_TEST_FLAG(pAd, fOP_STATUS_MEDIA_STATE_CONNECTED))
				))
			{
				RTUSBBulkOutDataPacket(pAd, EDCA_AC2_PIPE, pAd->NextBulkOutIndex[EDCA_AC2_PIPE]);
			}
		}
		if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL_4))
		{
			if (((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BSS_SCAN_IN_PROGRESS)) || 
				(!OPSTATUS_TEST_FLAG(pAd, fOP_STATUS_MEDIA_STATE_CONNECTED))
				))
			{
				RTUSBBulkOutDataPacket(pAd, EDCA_AC3_PIPE, pAd->NextBulkOutIndex[EDCA_AC3_PIPE]);
			}
		}

		/* 7. Null frame is the last*/
		else if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NULL))
		{
			if (!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BSS_SCAN_IN_PROGRESS))
			{
				RTUSBBulkOutNullFrame(pAd);
			}
		}

		/* 8. No data avaliable*/
		else
		{
			
		}
	}
#ifdef RALINK_ATE			
	else if((ATE_ON(pAd)) &&
			!RTMP_TEST_FLAG(pAd , fRTMP_ADAPTER_NEED_STOP_TX))
	{
		if (RTUSB_TEST_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_ATE))
		{
			ATE_RTUSBBulkOutDataPacket(pAd, EDCA_AC0_PIPE);
		}
	}	
#endif /* RALINK_ATE */

}

/*
	========================================================================
	
	Routine Description:
	Call from Reset action after BulkOut failed. 
	Arguments:

	Return Value:

	Note:
	
	========================================================================
*/
VOID	RTUSBCleanUpDataBulkOutQueue(
	IN	PRTMP_ADAPTER	pAd)
{
	UCHAR			Idx;			
	PHT_TX_CONTEXT	pTxContext;
	
	DBGPRINT(RT_DEBUG_TRACE, ("--->CleanUpDataBulkOutQueue\n"));

	for (Idx = 0; Idx < 4; Idx++)
	{
		pTxContext = &pAd->TxContext[Idx];
		
		pTxContext->CurWritePosition = pTxContext->NextBulkOutPosition;
		pTxContext->LastOne = FALSE;
		NdisAcquireSpinLock(&pAd->BulkOutLock[Idx]);
		pAd->BulkOutPending[Idx] = FALSE;
		NdisReleaseSpinLock(&pAd->BulkOutLock[Idx]);
	}
	
	DBGPRINT(RT_DEBUG_TRACE, ("<---CleanUpDataBulkOutQueue\n"));
}

/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:
	
	Note:
	
	========================================================================
*/
VOID	RTUSBCleanUpMLMEBulkOutQueue(
	IN	PRTMP_ADAPTER	pAd)
{
	DBGPRINT(RT_DEBUG_TRACE, ("--->CleanUpMLMEBulkOutQueue\n"));


	DBGPRINT(RT_DEBUG_TRACE, ("<---CleanUpMLMEBulkOutQueue\n"));
}


/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	
	Note:
	
	========================================================================
*/
VOID	RTUSBCancelPendingIRPs(
	IN	PRTMP_ADAPTER	pAd)
{
	RTUSBCancelPendingBulkInIRP(pAd);
	RTUSBCancelPendingBulkOutIRP(pAd);
}

/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note:
	
	========================================================================
*/
VOID	RTUSBCancelPendingBulkInIRP(
	IN	PRTMP_ADAPTER	pAd)
{
	PRX_CONTEXT		pRxContext;
	UINT			i;

	DBGPRINT_RAW(RT_DEBUG_TRACE, ("--->RTUSBCancelPendingBulkInIRP\n"));
	for ( i = 0; i < (RX_RING_SIZE); i++)
	{
		pRxContext = &(pAd->RxContext[i]);
		if(pRxContext->IRPPending == TRUE)
		{
			RTUSB_UNLINK_URB(pRxContext->pUrb);
			pRxContext->IRPPending = FALSE;
			pRxContext->InUse = FALSE;
			/*NdisInterlockedDecrement(&pAd->PendingRx);*/
			/*pAd->PendingRx--;*/
		}
	}
	DBGPRINT_RAW(RT_DEBUG_TRACE, ("<---RTUSBCancelPendingBulkInIRP\n"));
}


/*
	========================================================================
	
	Routine Description:

	Arguments:

	Return Value:

	Note:
	
	========================================================================
*/
VOID	RTUSBCancelPendingBulkOutIRP(
	IN	PRTMP_ADAPTER	pAd)
{
	PHT_TX_CONTEXT		pHTTXContext;
	PTX_CONTEXT			pMLMEContext;
	PTX_CONTEXT			pNullContext;
	PTX_CONTEXT			pPsPollContext;
	UINT				i, Idx;
/*	unsigned int 		IrqFlags;*/
/*	NDIS_SPIN_LOCK		*pLock;*/
/*	BOOLEAN				*pPending;*/
	

/*	pLock = &pAd->BulkOutLock[MGMTPIPEIDX];*/
/*	pPending = &pAd->BulkOutPending[MGMTPIPEIDX];*/

	for (Idx = 0; Idx < 4; Idx++)
	{
		pHTTXContext = &(pAd->TxContext[Idx]);

		if (pHTTXContext->IRPPending == TRUE)
		{

			/* Get the USB_CONTEXT and cancel it's IRP; the completion routine will itself*/
			/* remove it from the HeadPendingSendList and NULL out HeadPendingSendList*/
			/*	when the last IRP on the list has been	cancelled; that's how we exit this loop*/
			

			RTUSB_UNLINK_URB(pHTTXContext->pUrb);

			/* Sleep 200 microseconds to give cancellation time to work*/
			RTMPusecDelay(200);
		}

#ifdef RALINK_ATE
		pHTTXContext->bCopySavePad = 0;
		pHTTXContext->CurWritePosition = 0;
		pHTTXContext->CurWriteRealPos = 0;
		pHTTXContext->bCurWriting = FALSE;
		pHTTXContext->NextBulkOutPosition = 0;
		pHTTXContext->ENextBulkOutPosition = 0;
#endif /* RALINK_ATE */
		pAd->BulkOutPending[Idx] = FALSE;
	}

	/*RTMP_IRQ_LOCK(pLock, IrqFlags);*/
	for (i = 0; i < MGMT_RING_SIZE; i++)
	{
		pMLMEContext = (PTX_CONTEXT)pAd->MgmtRing.Cell[i].AllocVa;
		if(pMLMEContext && (pMLMEContext->IRPPending == TRUE))
		{

			/* Get the USB_CONTEXT and cancel it's IRP; the completion routine will itself*/
			/* remove it from the HeadPendingSendList and NULL out HeadPendingSendList*/
			/*	when the last IRP on the list has been	cancelled; that's how we exit this loop*/
			

			RTUSB_UNLINK_URB(pMLMEContext->pUrb);
			pMLMEContext->IRPPending = FALSE;
			
			/* Sleep 200 microsecs to give cancellation time to work*/
			RTMPusecDelay(200);
		}
	}
	pAd->BulkOutPending[MGMTPIPEIDX] = FALSE;
	/*RTMP_IRQ_UNLOCK(pLock, IrqFlags);*/

	pNullContext = &(pAd->NullContext);
	if (pNullContext->IRPPending == TRUE)
		RTUSB_UNLINK_URB(pNullContext->pUrb);

	pPsPollContext = &(pAd->PsPollContext);
	if (pPsPollContext->IRPPending == TRUE)
		RTUSB_UNLINK_URB(pPsPollContext->pUrb);

	for (Idx = 0; Idx < 4; Idx++)
	{
		NdisAcquireSpinLock(&pAd->BulkOutLock[Idx]);
		pAd->BulkOutPending[Idx] = FALSE;
		NdisReleaseSpinLock(&pAd->BulkOutLock[Idx]);
	}
}

#endif /* RTMP_MAC_USB */
