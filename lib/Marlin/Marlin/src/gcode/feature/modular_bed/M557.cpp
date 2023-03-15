/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2019 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "../../../inc/MarlinConfig.h"
#include "../../gcode.h"
#include "../../../module/temperature.h"

#if ENABLED(MODULAR_HEATBED)

#include "../../../module/modular_heatbed.h"

/**
 * M557: Set modular bed gradient parameters
 */
void GcodeSuite::M557() {

    // Set gradient cutoff
    if (parser.seen('C')) {
        float cutoff = parser.floatval('C');
        advanced_modular_bed->set_gradient_cutoff(cutoff);
    }

    // set gradient exponent
    if (parser.seen('E')) {
        float exponent = parser.floatval('E');
        advanced_modular_bed->set_gradient_exponent(exponent);
    }

    // recalculate gradients
    advanced_modular_bed->update_bedlet_temps(Temperature::getEnabledBedletMask(), thermalManager.degTargetBed());
}

#endif
