"""Offline checks of the landing page tape's expiry choice."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from site_tape_data import weeklies  # noqa: E402


class WeekliesTest(unittest.TestCase):
    def test_keeps_the_friday_of_each_week_and_drops_dailies(self):
        listed = ['261117', '261118', '261119', '261120', '261123', '261125', '261127', '261204']
        self.assertEqual(weeklies(listed), ['261120', '261127', '261204'])

    def test_takes_the_thursday_when_friday_is_a_holiday(self):
        # Good Friday, 2 April 2027: that week's weekly expires on Thursday 1 April.
        self.assertEqual(weeklies(['270329', '270331', '270401', '270409']), ['270401', '270409'])

    def test_spans_a_month_and_year_end(self):
        # New Year's Day 2027 is a Friday, so that week's weekly expires on 31 December.
        self.assertEqual(weeklies(['261228', '261230', '261231', '270104', '270108'])[:2], ['261231', '270108'])

    def test_drops_a_week_whose_last_listed_expiry_is_early(self):
        self.assertEqual(weeklies(['261123', '261125']), [])


if __name__ == '__main__':
    unittest.main()
