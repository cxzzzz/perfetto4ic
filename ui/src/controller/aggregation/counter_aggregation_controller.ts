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

import {Duration} from '../../base/time';
import {ColumnDef} from '../../common/aggregation_data';
import {Engine} from '../../common/engine';
import {pluginManager} from '../../common/plugins';
import {Area, Sorting} from '../../common/state';
import {globals} from '../../frontend/globals';
import {COUNTER_TRACK_KIND} from '../../tracks/counter';

import {AggregationController} from './aggregation_controller';

export class CounterAggregationController extends AggregationController {
  async createAggregateView(engine: Engine, area: Area) {
    await engine.query(`drop view if exists ${this.kind};`);

    const trackIds: (string|number)[] = [];
    for (const trackId of area.tracks) {
      const track = globals.state.tracks[trackId];
      if (track?.uri) {
        const trackInfo = pluginManager.resolveTrackInfo(track.uri);
        if (trackInfo?.kind === COUNTER_TRACK_KIND) {
          trackInfo.trackIds && trackIds.push(...trackInfo.trackIds);
        }
      }
    }
    if (trackIds.length === 0) return false;
    const duration = area.end - area.start;
    const durationSec = Duration.toSeconds(duration);

    const query = `create view ${this.kind} as select
    name,
    count(1) as count,
    sum(weighted_value)/${duration} as avg_value,
    sum(weighted_value)/sum(selected_dur_wo_0) as avg_value_wo_0,
    last as last_value,
    first as first_value,
    max(last) - min(first) as delta_value,
    round((max(last) - min(first))/${durationSec}, 2) as rate,
    min(value) as min_value,
    max(value) as max_value,
    sum(selected_cycle) as sum_selected_cycle,
    sum(selected_cycle_value) as sum_cycle_value,
    round((${area.end} - ${area.start})/clk_freq,0) as cycle
    from (select * ,
        selected_dur*value as weighted_value,
        round(selected_dur/(clk_freq+0.0),0) as selected_cycle,
        (case when value!=0 then round(selected_dur/(clk_freq+0.0),0) 
          else 0 end) as selected_cycle_wo_0,
        (case when value!=0 then selected_dur else 0 end) as selected_dur_wo_0,
        value*round(selected_dur/(clk_freq+0.0),0) as selected_cycle_value
        from (select *,
          (min(ts + dur, ${area.end}) - max(ts,${area.start})) as selected_dur,
          first_value(value) over
          (partition by track_id order by ts) as first,
          last_value(value) over
          (partition by track_id order by ts
              range between unbounded preceding and unbounded following) as last,
          args.int_value as clk_freq,
          (min(ts + dur, ${area.end}) - max(ts,${area.start})) as real_dur
          from experimental_counter_dur 
            join counter_track on track_id = counter_track.id 
            left join args on ("args."||counter_track.name) == args.key
          where (track_id in (${trackIds}))
          and (NOT (ts > ${area.end} OR ts + dur < ${area.start}))
        )
    )
    group by track_id`;

    await engine.query(query);
    return true;
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        kind: 'STRING',
        columnConstructor: Uint16Array,
        columnId: 'name',
      },
      {
        title: 'Cycle',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'cycle',

      },
      /*{
        title: 'Delta value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'delta_value',
      },
      {
        title: 'Rate /s',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'rate',
      },*/
      {
        title: 'Weighted avg value',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'avg_value',
        sum: true
      },
      {
        title: 'Weighted avg value(w/o 0)',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'avg_value_wo_0',
        sum: true
      },
      {
        title: 'Sum value',
        kind: 'Number',
        columnConstructor: Float64Array,
        columnId: 'sum_cycle_value',
        sum: true
        

      },
      {
        title: 'Min value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'min_value',
      },
      {
        title: 'Max value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'max_value',
      },      
      {
        title: 'First value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'first_value',
      },
      {
        title: 'Last value',
        kind: 'NUMBER',
        columnConstructor: Float64Array,
        columnId: 'last_value',
      },

    ];
  }

  async getExtra() {}

  getTabName() {
    return 'Counters';
  }

  getDefaultSorting(): Sorting {
    return {column: 'name', direction: 'DESC'};
  }
}
