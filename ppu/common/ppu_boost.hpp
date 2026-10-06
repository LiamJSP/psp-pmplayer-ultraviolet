#ifndef PPU_BOOST_HPP
#define PPU_BOOST_HPP

/* C++-only, header-only Boost policy. Include before any Boost header.
 * Keep the dependency reproducible across rolling-release host upgrades.
 * See docs/BOOST_INTEGRATION.md for the isolated header installation.
 * Do not disable thread support: PSP callbacks/workers still preempt us. */
#ifndef BOOST_ALL_NO_LIB
#define BOOST_ALL_NO_LIB 1
#endif
#ifndef BOOST_NO_EXCEPTIONS
#define BOOST_NO_EXCEPTIONS 1
#endif

#include <boost/version.hpp>

#if BOOST_VERSION != 108500
#error "PPU requires Boost 1.85.0 headers; see docs/BOOST_INTEGRATION.md"
#endif

#endif
