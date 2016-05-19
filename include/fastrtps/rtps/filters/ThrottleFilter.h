#ifndef THROTTLE_FILTER_H
#define THROTTLE_FILTER_H

#include <fastrtps/rtps/filters/FlowFilter.h>
#include <thread>

/*
 * Lets changes through. If any of the changes it last
 * allowed is sent, it will filter everything out for
 * a period in milliseconds.
 */
namespace eprosima{
namespace fastrtps{
namespace rtps{

class ThrottleFilter : public FlowFilter
{
   public:
   explicit ThrottleFilter(unsigned int throttlePeriodInMS);

   virtual std::vector<const CacheChange_t*> operator()(std::vector<const CacheChange_t*>);

   private:
   virtual void NotifyChangeSent(const CacheChange_t*);
   unsigned int m_lastThrottleTimeInMs;
   unsigned int m_throttlePeriodInMs;
   bool m_throttling;
   std::recursive_mutex m_mutex;
   std::vector<const CacheChange_t*> m_lastClearedChanges;
};

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima

#endif
