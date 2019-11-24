#pragma once

namespace pcpp
{

class DpdkCpp
{
private:
	DpdkCpp(){}
	void CheckAllPortsLinkStatus(unsigned int port_mask);

	int InitEal();
	int InitPortQueue();
	int InitAllPort();
	void LaunchAllLcore();
	void CloseAllPort();


	public:
	virtual ~DpdkCpp(){}
		static inline DpdkCpp& GetInstance()
		{
			static DpdkCpp instance;
			return instance;
		}

	int InitDpdk();

};


};
