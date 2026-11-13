import unittest

from tools.benchloop import families, nakbatches


class FamiliesTest(unittest.TestCase):
    def test_missing_results_still_give_every_line(self):
        planned = [x for _, generate in nakbatches.BATCHES.values() for x in generate()]
        self.assertTrue(families.asked(planned))
        found = families.analyse(planned, {})
        self.assertEqual(found['clock_hz'], families.MEASURED_CLOCK_HZ)
        heads = {line.split()[0] for line in found['summary']}
        for head in ('clock', 'stall', 'stall-wide', 'xmad-mul', 'icache', 'store2fma'):
            self.assertIn(head, heads)
        self.assertTrue(all(v['missing'] for v in found['store2fma'].values()))

    def test_a_run_of_other_batches_is_not_theirs(self):
        planned = [{'id': 'plain', 'kind': 'noop'}, {'id': 'other', 'kind': 'noop', 'meta': {'family': 'twod'}}]
        self.assertFalse(families.asked(planned))


class BatchFilesTest(unittest.TestCase):
    def test_files_are_the_generators_output(self):
        for stem in nakbatches.BATCHES:
            path = nakbatches.FOLDER / f'{stem}.json'
            self.assertTrue(path.is_file(), path)
        for stem, text in nakbatches.texts().items():
            self.assertEqual((nakbatches.FOLDER / f'{stem}.json').read_text(), text, stem)


if __name__ == '__main__':
    unittest.main()
