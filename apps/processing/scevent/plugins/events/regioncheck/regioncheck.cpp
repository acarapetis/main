/***************************************************************************
 * Copyright (C) gempa GmbH                                                *
 * All rights reserved.                                                    *
 * Contact: gempa GmbH (seiscomp-dev@gempa.de)                             *
 *                                                                         *
 * GNU Affero General Public License Usage                                 *
 * This file may be used under the terms of the GNU Affero                 *
 * Public License version 3.0 as published by the Free Software Foundation *
 * and appearing in the file LICENSE included in the packaging of this     *
 * file. Please review the following information to ensure the GNU Affero  *
 * Public License version 3.0 requirements will be met:                    *
 * https://www.gnu.org/licenses/agpl-3.0.html.                             *
 *                                                                         *
 * Other Usage                                                             *
 * Alternatively, this file may be used in accordance with the terms and   *
 * conditions contained in a signed written agreement between you and      *
 * gempa GmbH.                                                             *
 ***************************************************************************/


#define SEISCOMP_COMPONENT REGIONCHECK

#include <seiscomp/core/plugin.h>
#include <seiscomp/core/strings.h>
#include <seiscomp/logging/log.h>
#include <seiscomp/plugins/events/eventprocessor.h>
#include <seiscomp/datamodel/event.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/seismology/regions.h>
#include <seiscomp/geo/featureset.h>


ADD_SC_PLUGIN("Region check for events that sets the event type to \"outside of network interest\" "
              "if the location is outside preconfigured regions",
              "Jan Becker, Dirk Roessler, gempa GmbH <jabe@gempa.de>", 0, 1, 0)


using namespace std;
using namespace Seiscomp;
using namespace Seiscomp::DataModel;
using namespace Seiscomp::Geo;

bool getEventType(EventType &type, const GeoFeature::Attributes &attributes) {
	GeoFeature::Attributes::const_iterator it = attributes.find("eventType");
	if ( it == attributes.end() ) {
		return false;
	}
	if ( !type.fromString(it->second) ) {
		SEISCOMP_ERROR(" + evrc: got invalid event type: %s", it->second.c_str());
		return false;
	}
	return true;
}

bool getBnaAttributes(double &value, const GeoFeature::Attributes &attributes, const string &key) {
	GeoFeature::Attributes::const_iterator it = attributes.find(key);
	if ( it == attributes.end() ) {
		return false;
	}

	if ( !Seiscomp::Core::fromString(value,it->second) ) {
		return false;
	}
	return true;
}

class RegionCheckProcessor : public Seiscomp::Client::EventProcessor {
	// ----------------------------------------------------------------------
	// X'truction
	// ----------------------------------------------------------------------
	public:
		//! C'tor
		RegionCheckProcessor() {}


	// ----------------------------------------------------------------------
	// Public EventProcessor interface
	// ----------------------------------------------------------------------
	public:
		/**
		 * @brief Setup the processor and read settings from the passed
		 *        configuration.
		 * @param config The config object to read settings from
		 * @return Success flag
		 */
		bool setup(const Config::Config &config) {
			const GeoFeatureSet &featureSet = GeoFeatureSetSingleton::getInstance();
			const vector<GeoFeature*> &features = featureSet.features();

			_hasPositiveRegions = false;
			_hasNegativeRegions = false;

			try { _readEventTypeFromBNA = config.getBool("rc.readEventTypeFromBNA"); }
			catch ( ... ) {
				_readEventTypeFromBNA = false;
			}

			EventType etype;
			if ( !_readEventTypeFromBNA ) {
				try {
					if ( etype.fromString(config.getString("rc.eventTypePositive")) ) {
						_eventTypePositive = etype;
					}
					else {
						SEISCOMP_ERROR(" + evrc: Invalid event type in rc.eventTypePositive");
						return false;
					}
				}
				catch ( ... ) {
					SEISCOMP_DEBUG(" + evrc: event type not set in rc.eventTypePositive");
				}
			}

			try {
				if (  etype.fromString(config.getString("rc.eventTypeNegative")) ) {
					_eventTypeNegative = etype;
				}
				else {
					SEISCOMP_ERROR(" + evrc: Invalid event type in rc.eventTypeNegative");
					return false;
				}
			}
			catch ( ... ) {
				SEISCOMP_DEBUG(" + evrc: rc.eventTypeNegative not set - using default"
				               " for negative events: outside of network interest");
				_eventTypeNegative = OUTSIDE_OF_NETWORK_INTEREST;
			}

			try {
				vector<string> regions = config.getStrings("rc.regions");
				if ( regions.empty() )
					throw Core::ValueException();

				for ( size_t i = 0; i < regions.size(); ++i ) {
					string regionName = regions[i];
					Core::trim(regionName);

					if ( regionName.empty() ) {
						SEISCOMP_ERROR(" + evrc: empty region names are not allowed");
						return false;
					}

					if ( regionName == "accept" ) {
						_regions.push_back(RegionCheck(NULL, true));
						_hasPositiveRegions = true;
						SEISCOMP_DEBUG(" + evrc: %s - add world as positive region", regionName.c_str());
					}
					else if ( regionName == "!reject" || regionName == "reject" ) {
						_regions.push_back(RegionCheck(NULL, false));
						_hasNegativeRegions = true;
						SEISCOMP_DEBUG(" + evrc: %s - add world as negative region",regionName.c_str());
					}
					else {
						bool foundRegion = false;
						bool positiveRegion = true;

						if ( regionName[0] == '!' ) {
							positiveRegion = false;
							regionName.erase(0,1);
							Core::trim(regionName);
						}

						// Check for available BNA regions
						for ( vector<GeoFeature*>::const_iterator it = features.begin();
						      it != features.end(); ++it ) {
							string featureName = (*it)->name();
							Core::trim(featureName);

							if ( regionName != featureName ) continue;

							if ( foundRegion ) {
								SEISCOMP_ERROR(" + evrc: multiple definition of region '%s' in .bna file. "
								               "Check the parameter regions or the .bna files in the BNA directories!",
								               featureName.c_str());
								return false;
							}

							if ( !(*it)->closedPolygon() ) {
								SEISCOMP_ERROR(" + evrc: region '%s' unused. Polygon is not closed. "
								               "Check the .bna files in the BNA directories!",
								               featureName.c_str());
								return false;
							}

							SEISCOMP_DEBUG(" + evrc: add %s region '%s'",
							               positiveRegion ? "positive":"negative",
							               featureName.c_str());
							_regions.push_back(RegionCheck(*it, positiveRegion));

							if ( positiveRegion )
								_hasPositiveRegions = true;
							else
								_hasNegativeRegions = true;

							foundRegion = true;
						}

						if ( !foundRegion ) {
							SEISCOMP_ERROR(" + evrc: region '%s' has not been found",
							               regionName.c_str());
							return false;
						}
					}
				}
			}
			catch ( ... ) {
				SEISCOMP_ERROR(" + evrc: No regions configured, region check is useless");
				return false;
			}

			return true;
		}


		/**
		 * @brief Processes and modifies an event. It checks the current
		 *        location against the configured regions and sets the
		 *        type to OUTSIDE_OF_NETWORK_INTEREST if the location is not
		 *        inside in any of the regions.
		 * @param event The event to be processed
		 * @return Update flag: true, if the event has been updated, false
		 *         otherwise.
		 */
		bool process(Event *event) {
			Origin *org = Origin::Find(event->preferredOriginID());
			_setType = true;
			double depth = org->depth().value();

			SEISCOMP_DEBUG("evrc plugin: processing event %s",
			                 event->publicID().c_str());
			if ( !org ) {
				SEISCOMP_WARNING(" + evrc: no origin information found");
				return false;
			}

			try {
				if ( org->evaluationMode() == DataModel::MANUAL ){
					SEISCOMP_DEBUG(" + evrc: found %s preferred origin %s: "
					               "do not change the event status",
					               org->evaluationMode().toString(),
					               org->publicID().c_str());
					return false;
				}
			}
			catch ( ... ) {}

			GeoCoordinate location;

			try {
				location.set(org->latitude().value(), org->longitude().value());
			}
			catch ( ... ) {
				SEISCOMP_WARNING(" + evrc: no lat/lon information available");
				return false;
			}

			SEISCOMP_DEBUG(" + evrc: checking regions for location %f / %f / %.2f km", location.lat, location.lon, depth);

			OPT(EventType) currentType;
			OPT(EventType) eventType;
			EventType tmp;

			try {
				currentType = event->type();
			}
			catch ( ... ) {}

			bool isInside = false;
			_setType = true;
			for ( size_t i = 0; i < _regions.size(); ++i ) {
				bool isInsideRegion;
				SEISCOMP_DEBUG(" + evrc: checking region %s",
				              (_regions[i].first ? _regions[i].first->name().c_str() : "world"));

				if ( _regions[i].first ) {
					isInsideRegion = _regions[i].first->contains(location);

					// check also values in BNA header
					if ( _readEventTypeFromBNA && isInsideRegion ) {
						double value;
						if ( getBnaAttributes(value, _regions[i].first->attributes(), "minDepth") ) {
							if ( depth < value ) {
								isInsideRegion = false;
								SEISCOMP_DEBUG("  + evrc: event at %.2f shallower than minDepth", depth);
							}
							else {
								SEISCOMP_DEBUG("  + evrc: event fulfills minDepth");
							}
						}
						else {
							SEISCOMP_DEBUG("  + evrc: minDepth not found for region %s, not considered",
							               _regions[i].first->name().c_str());
						}

						if ( getBnaAttributes(value, _regions[i].first->attributes(), "maxDepth") ) {
							if ( depth > value ) {
								isInsideRegion = false;
								SEISCOMP_DEBUG("  + evrc: event at %.2f deeper than maxDepth", depth);
							}
							else {
								SEISCOMP_DEBUG("  + evrc: event fullfils maxDepth");
							}
						}
						else {
							SEISCOMP_DEBUG("  + evrc: maxDepth not found for region %s, not considered",
							               _regions[i].first->name().c_str());
						}
					}
				}
				else
					isInsideRegion = true;

				if ( _regions[i].second ) {
					// Positive region
					if ( isInsideRegion ) {
						_setType = true;
						// Inside of network interest
						isInside = true;
						SEISCOMP_DEBUG("  + evrc: %s in positive region '%s' matches",
						               event->publicID().c_str(),
						               (_regions[i].first ? _regions[i].first->name().c_str() : "world"));

						// for backwards compatibility of positive events: set the event type only if the
						// region name is not world
						if ( !_regions[i].first ) {
							eventType = Core::None;
							_setType = false;
						}
						else if ( _regions[i].first->name().empty() ) {
							eventType = Core::None;
							_setType = false;
						}
						else {
							// If there are possible negative regions following this
							// match then continue.
							if ( !_hasNegativeRegions ) {
								if ( _readEventTypeFromBNA ) {
									if ( getEventType(tmp, _regions[i].first->attributes()) ) {
										eventType = tmp;
									}
									else {
										SEISCOMP_ERROR("  + evrc: cannot set event type from BNA, check BNA header");
										_setType = false;
									}
								}
								break;
							}
						}
					}
				}
				else {
					// Negative region
					_setType = true;
					// Continue with checking as there might follow a positive
					// region that overrides again that result unless there
					// are only negative regions configured
					if ( !_hasPositiveRegions ) {
						if ( _readEventTypeFromBNA ) {
							if ( getEventType(tmp, _regions[i].first->attributes()) ) {
								eventType = tmp;
							}
							else {
								SEISCOMP_ERROR("  + evrc: cannot set event type from BNA, check BNA header");
								_setType = false;
								break;
							}
						}
						if ( isInsideRegion ) {
							isInside = false;
							SEISCOMP_DEBUG("  + evrc: %s inside negative region '%s' matches, setting event type negative",
							               event->publicID().c_str(),
							              (_regions[i].first ? _regions[i].first->name().c_str() : "world"));
						}
						else {
							isInside = true;
							SEISCOMP_ERROR("  + evrc: %s is outside the negative region '%s', setting event type positive",
							               event->publicID().c_str(),
							               (_regions[i].first ? _regions[i].first->name().c_str() : "world"));
						}
						break;
					}
				}
			}

			if ( !_setType ) {
				SEISCOMP_DEBUG(" + evrc: do not set type");
				return true;
			}

			// set event type
			if ( !isInside ) {
				SEISCOMP_DEBUG("  + evrc: event is negative");
				if ( _eventTypeNegative ) {
					eventType = _eventTypeNegative;
					SEISCOMP_DEBUG("  + evrc: event type from config: %s", _eventTypeNegative->toString());
				}
				else {
					eventType = Core::None;
				}
			}
			else {
				SEISCOMP_DEBUG("  + evrc: event is positive");
				if ( !_readEventTypeFromBNA ) {
					if ( _eventTypePositive ) {
						eventType = _eventTypePositive;
						SEISCOMP_DEBUG("  + evrc: event type from config: %s", _eventTypePositive->toString());
					}
					else {
						SEISCOMP_DEBUG("  + evrc: considering empty event type from eventTypePositive");
						eventType = Core::None;
					}
				}
			}

			if ( !currentType || (currentType && *currentType != eventType) ) {
				if ( eventType ) {
					SEISCOMP_DEBUG("  + evrc: setting type: %s", eventType->toString());
					event->setType(eventType);
				}
				else {
					SEISCOMP_DEBUG("  + evrc: no event type determined, unset type");
					event->setType(Core::None);
				}
				return true;
			}
			else {
				SEISCOMP_DEBUG("  + evrc: new event type equals old one - do not update");
			}

			return false;
		}


	// ----------------------------------------------------------------------
	// Private members
	// ----------------------------------------------------------------------
	private:
		typedef pair<GeoFeature*,bool> RegionCheck;
		vector<RegionCheck>       _regions;
		OPT(DataModel::EventType) _eventTypePositive;
		OPT(DataModel::EventType) _eventTypeNegative;
		OPT(double)               _minDepth;
		OPT(double)               _maxDepth;
		bool                      _hasPositiveRegions;
		bool                      _hasNegativeRegions;
		bool                      _setType;
		bool                      _readEventTypeFromBNA;
};


REGISTER_EVENTPROCESSOR(RegionCheckProcessor, "RegionCheck");
