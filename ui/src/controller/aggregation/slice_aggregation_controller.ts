// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {ColumnDef} from '../../common/aggregation_data';
import {Engine} from '../../common/engine';
import {pluginManager} from '../../common/plugins';
import {Area, Sorting} from '../../common/state';
import {globals} from '../../frontend/globals';
import {ASYNC_SLICE_TRACK_KIND} from '../../tracks/async_slices';
import {SLICE_TRACK_KIND} from '../../tracks/chrome_slices';

import {AggregationController} from './aggregation_controller';

export function getSelectedTrackIds(area: Area): number[] {
  const selectedTrackIds: number[] = [];
  for (const trackId of area.tracks) {
    const track = globals.state.tracks[trackId];
    // Track will be undefined for track groups.
    if (track?.uri !== undefined) {
      const trackInfo = pluginManager.resolveTrackInfo(track.uri);
      if (trackInfo?.kind === SLICE_TRACK_KIND) {
        trackInfo.trackIds && selectedTrackIds.push(...trackInfo.trackIds);
      }
      if (trackInfo?.kind === ASYNC_SLICE_TRACK_KIND) {
        trackInfo.trackIds && selectedTrackIds.push(...trackInfo.trackIds);
      }
    }
  }
  return selectedTrackIds;
}

export class SliceAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    const selectedTrackIds = getSelectedTrackIds(area);

    if (selectedTrackIds.length === 0) return false;

    const query = `create view ${this.kind} as
        SELECT
        (case when (name==thread_name) then name 
            else (thread_name||" "||name) end) as alias,
        sum(selected_dur) AS total_dur,
        sum(selected_dur)/count(1) as avg_dur,
        format("%.4f%%", sum(selected_dur)*100/${area.end - area.start}) as duty,
        count(1) as occurrences, 
        round((sum(selected_dur)+0.0)/clk_freq,0) as sum_selected_cycle,
        round(round(sum(selected_dur)/(clk_freq+0.0),0)/count(1),4) as avg_selected_cycle,
        round((${area.end} - ${area.start})/clk_freq,0) as cycle
        FROM (
          SELECT *,
            args.int_value as clk_freq,
            (min(ts + dur, ${area.end}) - max(ts,${area.start})) as selected_dur
            FROM experimental_slice_with_thread_and_process_info LEFT JOIN args on ((("args."||experimental_slice_with_thread_and_process_info.name) == args.key) OR (("args."||experimental_slice_with_thread_and_process_info.thread_name) == args.key))
            WHERE track_id IN (${selectedTrackIds}) AND (NOT (ts > ${area.end} OR ts + dur < ${area.start}))
        )
        group by name`;

    await engine.query(query);
    return true;
  }

  getTabName() {
    return 'Slices';
  }

  async getExtra() {}

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        kind: 'STRING',
        columnConstructor: Uint32Array,
        columnId: 'alias',
      },
      {
        title: 'Cycle',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'cycle',
      },
      {
        title: 'Occurrences',
        kind: 'NUMBER',
        columnConstructor: Uint32Array,
        columnId: 'occurrences',
        sum: true,
      },
      {
        title: 'Sum duration (ns)',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'total_dur',
        sum: true,
      },
      {
        title: 'Sum duration (cycle)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'sum_selected_cycle',
        sum: true,
      },
      {
        title: 'Avg duration (ns)',
        kind: 'TIMESTAMP_NS',
        columnConstructor: Float64Array,
        columnId: 'avg_dur',
        sum: true,
      },
      {
        title: 'Avg duration (cycle)',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'avg_selected_cycle',
        sum: true,
      },
      {
        title: 'Duty (%)',
        kind: 'STRING',
        columnConstructor: Uint32Array,
        columnId: 'duty',
      },
    ];
  }
}
