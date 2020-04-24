/*!
 * \file UDPCaster.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "UDPCaster.h"
#include "DataManager.h"

#include "../Share/StrUtil.hpp"
#include "../Share/WTSDataDef.hpp"
#include "../Share/WTSCollection.hpp"
#include "../Share/WTSContractInfo.hpp"
#include "../Share/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"


#define UDP_MSG_SUBSCRIBE	0x100
#define UDP_MSG_PUSHTICK	0x200

#pragma pack(push,1)
//UDP�����
typedef struct _UDPReqPacket
{
	uint32_t		_type;
	char			_data[1020];
} UDPReqPacket;

//UDPTick���ݰ�
typedef struct _UDPTickPacket
{
	uint32_t		_type;
	WTSTickStruct	_tick;
} UDPTickPacket;
#pragma pack(pop)

UDPCaster::UDPCaster()
	: m_tickQue(NULL)
	, m_bTerminated(false)
	, m_bdMgr(NULL)
	, m_dtMgr(NULL)
{
	
}


UDPCaster::~UDPCaster()
{
}

bool UDPCaster::init(WTSVariant* cfg, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)
{
	m_bdMgr = bdMgr;
	m_dtMgr = dtMgr;

	if (!cfg->getBoolean("active"))
		return false;

	WTSVariant* cfgBC = cfg->get("broadcast");
	if (cfgBC)
	{
		for (uint32_t idx = 0; idx < cfgBC->size(); idx++)
		{
			WTSVariant* cfgItem = cfgBC->get(idx);
			addBRecver(cfgItem->getCString("host"), cfgItem->getInt32("port"), cfgItem->getUInt32("type"));
		}
	}

	WTSVariant* cfgMC = cfg->get("multicast");
	if (cfgMC)
	{
		for (uint32_t idx = 0; idx < cfgMC->size(); idx++)
		{
			WTSVariant* cfgItem = cfgMC->get(idx);
			addMRecver(cfgItem->getCString("host"), cfgItem->getInt32("port"), cfgItem->getInt32("sendport"), cfgItem->getUInt32("type"));
		}
	}

	start(cfg->getInt32("bport"));

	return true;
}

void UDPCaster::start(int bport)
{
	if (!m_listFlatRecver.empty() || !m_listJsonRecver.empty() || !m_listRawRecver.empty())
	{
		m_sktBroadcast.reset(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0)));
		boost::asio::socket_base::broadcast option(true);
		m_sktBroadcast->set_option(option);
	}

	m_sktSubscribe.reset(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), bport)));

	do_receive();

	m_thrdIO.reset(new BoostThread([this](){
		try
		{
			m_ioservice.run();
		}
		catch(...)
		{
			m_ioservice.stop();
		}
	}));
}

void UDPCaster::stop()
{
	m_bTerminated = true;
	m_ioservice.stop();
	if (m_thrdIO)
		m_thrdIO->join();

	m_condCast.notify_all();
	if (m_thrdCast)
		m_thrdCast->join();
}

void UDPCaster::do_receive()
{
	m_sktSubscribe->async_receive_from(boost::asio::buffer(m_data, max_length), m_senderEP,
		[this](boost::system::error_code ec, std::size_t bytes_recvd)
	{
		if(ec)
		{
			do_receive();
			return;
		}

		if (bytes_recvd == sizeof(UDPReqPacket))
		{
			UDPReqPacket* req = (UDPReqPacket*)m_data;

			std::string data;
			//��������
			if (req->_type == UDP_MSG_SUBSCRIBE)
			{
				const StringVector& ay = StrUtil::split(req->_data, ",");
				std::string code, exchg;
				for(const std::string& fullcode : ay)
				{
					auto pos = fullcode.find(".");
					if (pos == std::string::npos)
						code = fullcode;
					else
					{
						code = fullcode.substr(pos + 1);
						exchg = fullcode.substr(0, pos);
					}
					WTSContractInfo* ct = m_bdMgr->getContract(code.c_str(), exchg.c_str());
					if (ct == NULL)
						continue;

					WTSTickData* curTick = m_dtMgr->getCurTick(code.c_str(), exchg.c_str());
					if(curTick == NULL)
						continue;

					std::string* data = new std::string();
					data->resize(sizeof(UDPTickPacket), 0);
					UDPTickPacket* pkt = (UDPTickPacket*)data->data();
					pkt->_type = req->_type;
					memcpy(&pkt->_tick, &curTick->getTickStruct(), sizeof(WTSTickStruct));
					curTick->release();
					m_sktSubscribe->async_send_to(
						boost::asio::buffer(*data, data->size()), m_senderEP,
						[this, data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/)
					{
						delete data;
						if (ec)
						{
							WTSLogger::error("UDP���ݷ���ʧ�ܣ�%s", ec.message().c_str());
						}
					});
				}
			}			
		}
		else
		{
			std::string* data = new std::string("Can not indentify the command");
			m_sktSubscribe->async_send_to(
				boost::asio::buffer(*data, data->size()), m_senderEP,
				[this, data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/)
			{
				delete data;
				if (ec)
				{
					WTSLogger::error("UDP���ݷ���ʧ�ܣ�%s", ec.message().c_str());
				}
			});
		}

		do_receive();
	});
}

bool UDPCaster::addBRecver(const char* remote, int port, int type /* = 0 */)
{
	try
	{
		boost::asio::ip::address_v4 addr = boost::asio::ip::address_v4::from_string(remote);
		UDPReceiverPtr item(new UDPReceiver(EndPoint(addr, port), type));
		if(type == 0)
			m_listFlatRecver.push_back(item);
		else if(type == 1)
			m_listJsonRecver.push_back(item);
		else if(type == 2)
			m_listRawRecver.push_back(item);
	}
	catch(...)
	{
		return false;
	}

	return true;
}


bool UDPCaster::addMRecver(const char* remote, int port, int sendport, int type /* = 0 */)
{
	try
	{
		boost::asio::ip::address_v4 addr = boost::asio::ip::address_v4::from_string(remote);
		UDPReceiverPtr item(new UDPReceiver(EndPoint(addr, port), type));
		UDPSocketPtr sock(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sendport)));
		boost::asio::ip::multicast::join_group option(item->_ep.address());
		sock->set_option(option);
		if(type == 0)
			m_listFlatGroup.push_back(std::make_pair(sock, item));
		else if(type == 1)
			m_listJsonGroup.push_back(std::make_pair(sock, item));
		else if(type == 2)
			m_listRawGroup.push_back(std::make_pair(sock, item));
	}
	catch(...)
	{
		return false;
	}

	return true;
}

void UDPCaster::broadcast(WTSTickData* curTick)
{
	if(m_sktBroadcast == NULL || curTick == NULL || m_bTerminated)
		return;

	{
		BoostUniqueLock lock(m_mtxCast);
		if (m_tickQue == NULL)
			m_tickQue = WTSQueue::create();

		m_tickQue->push(curTick, true);
	}

	if(m_thrdCast == NULL)
	{
		m_thrdCast.reset(new BoostThread([this](){

			while (!m_bTerminated)
			{
				if(m_tickQue == NULL || m_tickQue->empty())
				{
					BoostUniqueLock lock(m_mtxCast);
					m_condCast.wait(lock);
					continue;
				}

				WTSTickData* curTick = NULL;
				{
					BoostUniqueLock lock(m_mtxCast);
					curTick = (WTSTickData*)m_tickQue->front(true);
					m_tickQue->pop();
				}

				if(curTick == NULL)
					continue;

				//ֱ�ӹ㲥
				if (!m_listRawGroup.empty() || !m_listRawRecver.empty())
				{
					std::string buf_raw;
					buf_raw.resize(sizeof(UDPTickPacket));
					UDPTickPacket* pack = (UDPTickPacket*)buf_raw.data();
					pack->_type = UDP_MSG_PUSHTICK;
					memcpy(&pack->_tick, &curTick->getTickStruct(), sizeof(WTSTickStruct));
					//memcpy((void*)buf_raw.data(), &curTick->getTickStruct(), sizeof(WTSTickStruct));
					//�㲥
					for (auto it = m_listRawRecver.begin(); it != m_listRawRecver.end(); it++)
					{
						const UDPReceiverPtr& receiver = (*it);
						m_sktBroadcast->send_to(boost::asio::buffer(buf_raw), receiver->_ep);
					}

					//�鲥
					for (auto it = m_listRawGroup.begin(); it != m_listRawGroup.end(); it++)
					{
						const MulticastPair& item = *it;
						it->first->send_to(boost::asio::buffer(buf_raw), item.second->_ep);
					}
				}

				curTick->release();
			}
		}));
	}
	else
	{
		m_condCast.notify_all();
	}

	//���ı���ʽ
	/*
	if(!m_listFlatRecver.empty() || !m_listFlatGroup.empty())
	{
		uint32_t curTime = curTick->actiontime()/1000;
		char buf_flat[2048] = {0};
		char *str = buf_flat;
		//���ڣ�ʱ�䣬��ۣ����ۣ����룬���¼ۣ������ߣ��ͣ���ᣬ��ᣬ���֣����֣��֣ܳ����֣���λ[��x�ۣ���x������x�ۣ���x��]
		str += sprintf(str, "%04d.%02d.%02d,", 
			curTick->actiondate()/10000, curTick->actiondate()%10000/100, curTick->actiondate()%100);
		str += sprintf(str, "%02d:%02d:%02d,", 
			curTime/10000, curTime%10000/100, curTime%100);
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->bidprice(0)));
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->askprice(0)));
		str += sprintf(str, "%s,", curTick->code());

		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->price()));
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->open()));
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->high()));
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->low()));
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->settlepx()));
		str += sprintf(str, "%.2f,", PRICE_INT_TO_DOUBLE(curTick->preclose()));

		str += sprintf(str, "%u,", curTick->totalvolumn());
		str += sprintf(str, "%u,", curTick->volumn());
		str += sprintf(str, "%u,", curTick->openinterest());
		str += sprintf(str, "%d,", curTick->additional());

		for(int i = 0; i < 5; i++)
		{
			str += sprintf(str, "%.2f,%u,", PRICE_INT_TO_DOUBLE(curTick->bidprice(i)), curTick->bidqty(i));
			str += sprintf(str, "%.2f,%u,", PRICE_INT_TO_DOUBLE(curTick->askprice(i)), curTick->askqty(i));
		}

		for(auto it = m_listFlatRecver.begin(); it != m_listFlatRecver.end(); it++)
		{
			const UDPReceiverPtr& receiver = (*it);
			m_sktBroadcast->send_to(boost::asio::buffer(buf_flat, strlen(buf_flat)), receiver->_ep);
			sendTicks++;
			sendBytes += strlen(buf_flat);
		}

		//�鲥
		for(auto it = m_listFlatGroup.begin(); it != m_listFlatGroup.end(); it++)
		{
			const MulticastPair& item = *it;
			it->first->send_to(boost::asio::buffer(buf_flat, strlen(buf_flat)), item.second->_ep);
			sendTicks++;
			sendBytes += strlen(buf_flat);
		}
	}
	

	//json��ʽ
	if(!m_listJsonRecver.empty() || !m_listJsonGroup.empty())
	{
		datasvr::TickData newTick;
		newTick.set_market(curTick->market());
		newTick.set_code(curTick->code());

		newTick.set_price(curTick->price());
		newTick.set_open(curTick->open());
		newTick.set_high(curTick->high());
		newTick.set_low(curTick->low());
		newTick.set_preclose(curTick->preclose());
		newTick.set_settlepx(curTick->settlepx());

		newTick.set_totalvolumn(curTick->totalvolumn());
		newTick.set_volumn(curTick->volumn());
		newTick.set_totalmoney(curTick->totalturnover());
		newTick.set_money(curTick->turnover());
		newTick.set_openinterest(curTick->openinterest());
		newTick.set_additional(curTick->additional());

		newTick.set_tradingdate(curTick->tradingdate());
		newTick.set_actiondate(curTick->actiondate());
		newTick.set_actiontime(curTick->actiontime());

		for(int i = 0; i < 10; i++)
		{
			if(curTick->bidprice(i) == 0 && curTick->askprice(i) == 0)
				break;

			newTick.add_bidprices(curTick->bidprice(i));
			newTick.add_bidqty(curTick->bidqty(i));

			newTick.add_askprices(curTick->askprice(i));
			newTick.add_askqty(curTick->askqty(i));
		}
		const std::string& buf_json =  pb2json(newTick);

		//�㲥
		for(auto it = m_listJsonRecver.begin(); it != m_listJsonRecver.end(); it++)
		{
			const UDPReceiverPtr& receiver = (*it);
			m_sktBroadcast->send_to(boost::asio::buffer(buf_json), receiver->_ep);
			sendTicks++;
			sendBytes += buf_json.size();
		}

		//�鲥
		for(auto it = m_listJsonGroup.begin(); it != m_listJsonGroup.end(); it++)
		{
			const MulticastPair& item = *it;
			it->first->send_to(boost::asio::buffer(buf_json), item.second->_ep);
			sendTicks++;
			sendBytes += buf_json.size();
		}
	}
	*/
}

void UDPCaster::handle_send_broad(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if(error)
	{
		WTSLogger::error("����㲥ʧ�ܣ�Ŀ���ַ��%s��������Ϣ��%s", ep.address().to_string().c_str(), error.message().c_str());
	}
}

void UDPCaster::handle_send_multi(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if(error)
	{
		WTSLogger::error("����ಥʧ�ܣ�Ŀ���ַ��%s��������Ϣ��%s", ep.address().to_string().c_str(), error.message().c_str());
	}
}
