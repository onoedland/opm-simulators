/*
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#include <flow/flow_ebos_oilwater_polymer_mech_degradation.hpp>

#include <opm/material/common/ResetLocale.hpp>
#include <opm/models/blackoil/blackoiltwophaseindices.hh>

#include <opm/grid/CpGrid.hpp>
#include <opm/simulators/flow/SimulatorFullyImplicitBlackoilEbos.hpp>
#include <opm/simulators/flow/FlowMainEbos.hpp>

#if HAVE_DUNE_FEM
#include <dune/fem/misc/mpimanager.hh>
#else
#include <dune/common/parallel/mpihelper.hh>
#endif

namespace Opm {
namespace Properties {
namespace TTag {
struct EclFlowOilWaterPolymerMechanicalDegradationProblem {
    using InheritsFrom = std::tuple<EclFlowProblem>;
};
}
template<class TypeTag>
struct EnablePolymer<TypeTag, TTag::EclFlowOilWaterPolymerMechanicalDegradationProblem> {
    static constexpr bool value = true;
};
template<class TypeTag>
struct EnablePolymerMW<TypeTag, TTag::EclFlowOilWaterPolymerMechanicalDegradationProblem> {
    static constexpr bool value = true;
};
template<class TypeTag>
struct EnablePolymerMechanicalDegradation<TypeTag, TTag::EclFlowOilWaterPolymerMechanicalDegradationProblem> {
    static constexpr bool value = true;
};
//! The indices required by the model
// For this case, there will be two primary variables introduced for the polymer
// polymer concentration and polymer molecular weight
template<class TypeTag>
struct Indices<TypeTag, TTag::EclFlowOilWaterPolymerMechanicalDegradationProblem>
{
private:
    // it is unfortunately not possible to simply use 'TypeTag' here because this leads
    // to cyclic definitions of some properties. if this happens the compiler error
    // messages unfortunately are *really* confusing and not really helpful.
    using BaseTypeTag = TTag::EclFlowProblem;
    using FluidSystem = GetPropType<BaseTypeTag, Properties::FluidSystem>;

public:
    typedef Opm::BlackOilTwoPhaseIndices<0,
                                         0,
                                         2,
                                         0,
                                         getPropValue<TypeTag, Properties::EnableFoam>(),
                                         getPropValue<TypeTag, Properties::EnableBrine>(),
                                         /*PVOffset=*/0,
                                         /*disabledCompIdx=*/FluidSystem::gasCompIdx> type;
};
}}

namespace Opm {
/* void flowEbosOilWaterPolymerMechanicalDegradationSetDeck(Deck& deck, EclipseState& eclState)
{
    using TypeTag = Properties::TTag::EclFlowOilWaterPolymerMechanicalDegradationProblem;
    using Vanguard = GetPropType<TypeTag, Properties::Vanguard>;

    Vanguard::setExternalDeck(std::move(deck, &eclState));
} */

// ----------------- Main program -----------------
int flowEbosOilWaterPolymerMechanicalDegradationMain(int argc, char** argv, bool outputCout, bool outputFiles)
{
    // we always want to use the default locale, and thus spare us the trouble
    // with incorrect locale settings.
    Opm::resetLocale();

#if HAVE_DUNE_FEM
    Dune::Fem::MPIManager::initialize(argc, argv);
#else
    Dune::MPIHelper::instance(argc, argv);
#endif

    Opm::FlowMainEbos<Properties::TTag::EclFlowOilWaterPolymerMechanicalDegradationProblem>
        mainfunc {argc, argv, outputCout, outputFiles};
    return mainfunc.execute();
}

}
