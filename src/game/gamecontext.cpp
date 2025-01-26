void CGameContext::SendTuningParams(int ClientID)
{
	CheckPureTuning();

	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = (int *)&m_Tuning;
	for(unsigned i = 0; i < sizeof(m_Tuning)/sizeof(int); i++)
	{
		// Update laser reach and bounce num from config
		if (i == m_Tuning.LaserReach.id)
			m_Tuning.m_LaserReach = m_Config->m_SvLaserReach;
		else if (i == m_Tuning.LaserBounceNum.id)
			m_Tuning.m_LaserBounceNum = m_Config->m_SvLaserBounceNum;

		Msg.AddInt(pParams[i]);
	}
	
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
} 