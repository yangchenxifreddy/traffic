#ifndef PCAPPP_DPDK_DEVICE_LIST
#define PCAPPP_DPDK_DEVICE_LIST

#include "SystemUtils.h"
#include "DpdkDevice.h"
#include "Logger.h"
#include <vector>

/**
 * @file
 * For details about PcapPlusPlus support for DPDK see DpdkDevice.h file description
 */
class SNDpdkWorkerThread
{
public:
	/**
	 * A virtual d'tor. Can be overridden by child class if needed
	 */
	virtual ~SNDpdkWorkerThread() {}

	/**
	 * An abstract method that must be implemented by child class. It's the indication for the worker to start running
	 * @param[in] coreId The core ID the worker is running on (should be returned in getCoreId() )
	 * @return True if all went well or false otherwise
	 */
	virtual bool run(uint32_t coreId) = 0;

	/**

	 * An abstract method that must be implemented by child class. It's the indication for the worker to stop running. After
	 * this method is called the caller expects the worker to stop running as fast as possible
	 */
	virtual void stop() = 0;

	/**
	 * An abstract method that must be implemented by child class. Get the core ID the worker is running on (as sent to the run() method
	 * as a parameter)
	 * @return The core ID the worker is running on
	 */
	virtual uint32_t getCoreId() = 0;
};

 int dpdkmain();




#endif /* PCAPPP_DPDK_DEVICE_LIST */
