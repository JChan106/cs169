/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014 Universidad de la República - Uruguay
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Matias Richart <mrichart@fing.edu.uy>
 */

#include "mparf-wifi-manager.h"
#include "wifi-phy.h"
#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/trace-source-accessor.h"

#define Min(a,b) ((a < b) ? a : b)

NS_LOG_COMPONENT_DEFINE ("ns3::MParfWifiManager");

namespace ns3 {

/**
 * Hold per-remote-station state for PARF Wifi manag/Per.
 *
 * This struct extends from WifiRemoteStation struct to hold additional
 * information required by the PARF Wifi manager
 */
struct MParfWifiRemoteStation : public WifiRemoteStation
{
  uint32_t m_nAttempt;       //!< Number of transmission attempts.
  uint32_t m_nSuccess;       //!< Number of successful transmission attempts.
  uint32_t m_nFail;          //!< Number of failed transmission attempts.
  bool m_usingRecoveryRate;  //!< If using recovery rate.
  bool m_usingRecoveryPower; //!< If using recovery power.
  uint32_t m_nRetry;         //!< Number of transmission retries.
  uint32_t m_currentRate;    //!< Current rate used by the remote station.
  uint8_t m_currentPower;    //!< Current power used by the remote station.
  uint32_t m_PowerStepSize;
  uint32_t m_nSupported;     //!< Number of supported rates by the remote station.
  bool m_initialized;        //!< For initializing variables.
};

NS_OBJECT_ENSURE_REGISTERED (MParfWifiManager);

TypeId
MParfWifiManager::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MParfWifiManager")
    .SetParent<WifiRemoteStationManager> ()
    .SetGroupName ("Wifi")
    .AddConstructor<MParfWifiManager> ()
    .AddAttribute ("AttemptThreshold",
                   "The minimum number of transmission attempts to try a new power or rate.",
                   UintegerValue (15),
                   MakeUintegerAccessor (&MParfWifiManager::m_attemptThreshold),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("SuccessThreshold",
                   "The minimum number of successful transmissions to try a new power or rate.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&MParfWifiManager::m_successThreshold),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("PowerStepSize",
		   "Increment transmit power level by this",
		   UintegerValue(1),
                   MakeUintegerAccessor (&MParfWifiManager::m_PowerStepSize),
		   MakeUintegerChecker<uint32_t> ())
    .AddTraceSource ("PowerChange",
                     "The transmission power has change",
                     MakeTraceSourceAccessor (&MParfWifiManager::m_powerChange),
                     "ns3::WifiRemoteStationManager::PowerChangeTracedCallback")
    .AddTraceSource ("RateChange",
                     "The transmission rate has change",
                     MakeTraceSourceAccessor (&MParfWifiManager::m_rateChange),
                     "ns3::WifiRemoteStationManager::RateChangeTracedCallback")
  ;
  return tid;
}

MParfWifiManager::MParfWifiManager ()
{
  NS_LOG_FUNCTION (this);
}

MParfWifiManager::~MParfWifiManager ()
{
  NS_LOG_FUNCTION (this);
}

void
MParfWifiManager::SetupPhy (Ptr<WifiPhy> phy)
{
  m_minPower = phy->GetTxPowerStart ();
  m_maxPower = phy->GetTxPowerEnd ();
  WifiRemoteStationManager::SetupPhy (phy);
}

WifiRemoteStation *
MParfWifiManager::DoCreateStation (void) const
{
  NS_LOG_FUNCTION (this);
  MParfWifiRemoteStation *station = new MParfWifiRemoteStation ();

  station->m_nSuccess = 0;
  station->m_nFail = 0;
  station->m_usingRecoveryRate = false;
  station->m_usingRecoveryPower = false;
  station->m_initialized = false;
  station->m_nRetry = 0;
  station->m_nAttempt = 0;

  NS_LOG_DEBUG ("create station=" << station << ", timer=" << station->m_nAttempt
                                  << ", rate=" << station->m_currentRate << ", power=" << (int)station->m_currentPower);

  return station;
}

void
MParfWifiManager::CheckInit (MParfWifiRemoteStation *station)
{
  if (!station->m_initialized)
    {
      station->m_nSupported = GetNSupported (station);
      station->m_currentRate = station->m_nSupported - 1;
      station->m_currentPower = m_maxPower;
      m_powerChange (station->m_currentPower, station->m_state->m_address);
      m_rateChange (station->m_currentRate, station->m_state->m_address);
      station->m_initialized = true;
    }
}

void
MParfWifiManager::DoReportRtsFailed (WifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
}

/**
 * \internal
 * It is important to realize that "recovery" mode starts after failure of
 * the first transmission after a rate increase and ends at the first successful
 * transmission. Specifically, recovery mode spans retransmissions boundaries.
 * Fundamentally, ARF handles each data transmission independently, whether it
 * is the initial transmission of a packet or the retransmission of a packet.
 * The fundamental reason for this is that there is a backoff between each data
 * transmission, be it an initial transmission or a retransmission.
 */
void
MParfWifiManager::DoReportDataFailed (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);
  MParfWifiRemoteStation *station = (MParfWifiRemoteStation *)st;
  CheckInit (station);
  station->m_nAttempt++;
  station->m_nFail++;
  station->m_nRetry++;
  station->m_nSuccess = 0;

  NS_LOG_DEBUG ("station=" << station << " data fail retry=" << station->m_nRetry << ", timer=" << station->m_nAttempt
                           << ", rate=" << station->m_currentRate << ", power=" << (int)station->m_currentPower);
  if (station->m_usingRecoveryRate)
    {
      NS_ASSERT (station->m_nRetry >= 1);
      if (station->m_nRetry == 1)
        {
          //need recovery fallback
          if (station->m_currentRate != 0)
            {
              NS_LOG_DEBUG ("station=" << station << " dec rate");
              station->m_currentRate--;
              m_rateChange (station->m_currentRate, station->m_state->m_address);
              station->m_usingRecoveryRate = false;
            }
        }
      station->m_nAttempt = 0;
    }
  else if (station->m_usingRecoveryPower)
    {
      NS_ASSERT (station->m_nRetry >= 1);
      if (station->m_nRetry == 1)
        {
          //need recovery fallback
          if (station->m_currentPower < m_maxPower)
            {
              NS_LOG_DEBUG ("station=" << station << " inc power");
              //station->m_currentPower++;
              //if (station->m_currentPower + station->m_PowerStepSize > m_maxPower) {
	 	//station->m_currentPower = m_maxPower;
	      //}
 	      //else {
             //	station->m_currentPower += station->m_PowerStepSize;
 	      //}
	      station->m_currentPower = Min(station->m_currentPower + m_PowerStepSize, m_maxPower);
              m_powerChange (station->m_currentPower, station->m_state->m_address);
              station->m_usingRecoveryPower = false;
            }
        }
      station->m_nAttempt = 0;
    }
  else
    {
      NS_ASSERT (station->m_nRetry >= 1);
      if (((station->m_nRetry - 1) % 2) == 1)
        {
          //need normal fallback
          if (station->m_currentPower == m_maxPower)
            {
              if (station->m_currentRate != 0)
                {
                  NS_LOG_DEBUG ("station=" << station << " dec rate");
                  station->m_currentRate--;
                  m_rateChange (station->m_currentRate, station->m_state->m_address);
                }
            }
          else
            {
              NS_LOG_DEBUG ("station=" << station << " inc power");
              //station->m_currentPower++;
              //if (station->m_currentPower + station->m_PowerStepSize > m_maxPower) {
	 	//station->m_currentPower = m_maxPower;
	      //}
 	      //else {
             //	station->m_currentPower += station->m_PowerStepSize;
 	      //}
	      station->m_currentPower = Min(station->m_currentPower + m_PowerStepSize, m_maxPower);

 	      //station->m_currentPower = Min(
              m_powerChange (station->m_currentPower, station->m_state->m_address);
            }
        }
      if (station->m_nRetry >= 2)
        {
          station->m_nAttempt = 0;
        }
    }
}

void
MParfWifiManager::DoReportRxOk (WifiRemoteStation *station,
                               double rxSnr, WifiMode txMode)
{
  NS_LOG_FUNCTION (this << station << rxSnr << txMode);
}

void MParfWifiManager::DoReportRtsOk (WifiRemoteStation *station,
                                     double ctsSnr, WifiMode ctsMode, double rtsSnr)
{
  NS_LOG_FUNCTION (this << station << ctsSnr << ctsMode << rtsSnr);
  NS_LOG_DEBUG ("station=" << station << " rts ok");
}

void MParfWifiManager::DoReportDataOk (WifiRemoteStation *st,
                                      double ackSnr, WifiMode ackMode, double dataSnr)
{
  NS_LOG_FUNCTION (this << st << ackSnr << ackMode << dataSnr);
  MParfWifiRemoteStation *station = (MParfWifiRemoteStation *) st;
  CheckInit (station);
  station->m_nAttempt++;
  station->m_nSuccess++;
  station->m_nFail = 0;
  station->m_usingRecoveryRate = false;
  station->m_usingRecoveryPower = false;
  station->m_nRetry = 0;
  NS_LOG_DEBUG ("station=" << station << " data ok success=" << station->m_nSuccess << ", timer=" << station->m_nAttempt << ", rate=" << station->m_currentRate << ", power=" << (int)station->m_currentPower);
  if ((station->m_nSuccess == m_successThreshold
       || station->m_nAttempt == m_attemptThreshold)
      && (station->m_currentRate < (station->m_state->m_operationalRateSet.size () - 1)))
    {
      NS_LOG_DEBUG ("station=" << station << " inc rate");
      station->m_currentRate++;
      m_rateChange (station->m_currentRate, station->m_state->m_address);
      station->m_nAttempt = 0;
      station->m_nSuccess = 0;
      station->m_usingRecoveryRate = true;
    }
  else if (station->m_nSuccess == m_successThreshold || station->m_nAttempt == m_attemptThreshold)
    {
      //we are at the maximum rate, we decrease power
      if (station->m_currentPower != m_minPower)
        {
          NS_LOG_DEBUG ("station=" << station << " dec power");
          station->m_currentPower--;
          m_powerChange (station->m_currentPower, station->m_state->m_address);
        }
      station->m_nAttempt = 0;
      station->m_nSuccess = 0;
      station->m_usingRecoveryPower = true;
    }
}

void
MParfWifiManager::DoReportFinalRtsFailed (WifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
}

void
MParfWifiManager::DoReportFinalDataFailed (WifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
}

WifiTxVector
MParfWifiManager::DoGetDataTxVector (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);
  MParfWifiRemoteStation *station = (MParfWifiRemoteStation *) st;
  uint32_t channelWidth = GetChannelWidth (station);
  if (channelWidth > 20 && channelWidth != 22)
    {
      //avoid to use legacy rate adaptation algorithms for IEEE 802.11n/ac
      channelWidth = 20;
    }
  CheckInit (station);
  return WifiTxVector (GetSupported (station, station->m_currentRate), station->m_currentPower, GetLongRetryCount (station), false, 1, 0, channelWidth, GetAggregation (station), false);
}

WifiTxVector
MParfWifiManager::DoGetRtsTxVector (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);
  /// \todo we could/should implement the Arf algorithm for
  /// RTS only by picking a single rate within the BasicRateSet.
  MParfWifiRemoteStation *station = (MParfWifiRemoteStation *) st;
  uint32_t channelWidth = GetChannelWidth (station);
  if (channelWidth > 20 && channelWidth != 22)
    {
      //avoid to use legacy rate adaptation algorithms for IEEE 802.11n/ac
      channelWidth = 20;
    }
  WifiTxVector rtsTxVector;
  if (GetUseNonErpProtection () == false)
    {
      rtsTxVector = WifiTxVector (GetSupported (station, 0), GetDefaultTxPowerLevel (), GetShortRetryCount (station), false, 1, 0, channelWidth, GetAggregation (station), false);
    }
  else
    {
      rtsTxVector = WifiTxVector (GetNonErpSupported (station, 0), GetDefaultTxPowerLevel (), GetShortRetryCount (station), false, 1, 0, channelWidth, GetAggregation (station), false);
    }
  return rtsTxVector;
}

bool
MParfWifiManager::IsLowLatency (void) const
{
  NS_LOG_FUNCTION (this);
  return true;
}

} //namespace ns3
