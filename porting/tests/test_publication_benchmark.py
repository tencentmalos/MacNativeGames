# SPDX-License-Identifier: MIT
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'android/tools'))
from benchmark_publication import stable_cadence,soak_progress


def samples(rates):
    frame=0;result=[dict(begin=0,end=0,renderer=dict(frame='0',sample_age_ms='0'))]
    for i,rate in enumerate(rates):
        frame+=rate
        result.append(dict(begin=i+1,end=i+1,renderer=dict(frame=str(frame),sample_age_ms='0')))
    return result


class StabilityTests(unittest.TestCase):
    def test_fixed_complete_window_required(self):
        self.assertFalse(stable_cadence(samples([30]*59)))
        self.assertTrue(stable_cadence(samples([30]*60)))

    def test_recovery_must_finish_before_comparison(self):
        self.assertFalse(stable_cadence(samples([37]*40+[30]*20)))
        self.assertTrue(stable_cadence(samples([37]*60+[30]*60)))

    def test_jitter_and_drift_are_distinct(self):
        self.assertTrue(stable_cadence(samples([29,30]*30)))
        self.assertFalse(stable_cadence(samples([30]*30+[29]*30)))

    def test_interrupted_soak_retains_only_observed_prefix(self):
        empty=soak_progress([],300)
        self.assertEqual(empty['state'],'interrupted')
        self.assertNotIn('fps',empty)
        partial=soak_progress(samples([30]*167),300)
        self.assertEqual(partial['state'],'interrupted')
        self.assertEqual(partial['elapsed_s'],167)
        self.assertEqual(partial['successful_presents'],5010)

    def test_full_duration_still_requires_successful_final_checks(self):
        rows=samples([30]*300)
        self.assertEqual(soak_progress(rows,300)['state'],'interrupted')
        self.assertEqual(soak_progress(rows,300,complete=True)['state'],'complete')


if __name__=='__main__':unittest.main()
