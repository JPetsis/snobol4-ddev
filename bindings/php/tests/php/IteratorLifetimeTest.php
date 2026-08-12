<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Pattern;
use Snobol\SearchIterator;
use Snobol\SplitIterator;

/**
 * Iterator lifetime and contract tests (fix-php-binding-scan-findings
 * group 3):
 * - iterators own references to the pattern and subject (no use-after-free
 *   when the caller drops its own references)
 * - SplitIterator falls back to the general search for non-literal
 *   delimiters instead of silently emitting the whole subject
 * - rewind() consults the core even for empty subjects (zero-width
 *   patterns still match) and clears stale matches on failure
 */
final class IteratorLifetimeTest extends TestCase
{
    public function testSearchIteratorKeepsPatternAlive(): void
    {
        $it = SearchIterator::fromPattern(Pattern::fromString("'a'"), 'aba');
        // Drop every external reference.
        gc_collect_cycles();

        $this->assertSame([0, 2], $this->collectSearchStarts($it));
    }

    public function testSearchIteratorKeepsSubjectAlive(): void
    {
        $subject = 'aba';
        $it = SearchIterator::fromPattern(Pattern::fromString("'a'"), $subject);
        $subject = 'zzz'; // reassign; the iterator keeps the original string
        gc_collect_cycles();

        $this->assertSame([0, 2], $this->collectSearchStarts($it));
    }

    public function testSplitIteratorKeepsPatternAndSubjectAlive(): void
    {
        $pattern = Pattern::fromString("','");
        $subject = 'a,b,c';
        $it = SplitIterator::fromPattern($pattern, $subject);
        $pattern = null;
        $subject = 'zzz';
        gc_collect_cycles();

        $this->assertSame(['a', 'b', 'c'], iterator_to_array($it));
    }

    public function testSplitIteratorNonLiteralSpanDelimiter(): void
    {
        $p = Pattern::fromString("SPAN(' ')");
        $it = SplitIterator::fromPattern($p, 'a b  c');

        // Must equal the eager searchSplit result — not the whole subject.
        $this->assertSame($p->searchSplit('a b  c'), iterator_to_array($it));
        $this->assertSame(['a', 'b', 'c'], iterator_to_array($it));
    }

    public function testSplitIteratorAlternationDelimiter(): void
    {
        $p = Pattern::fromString("',' | ';'");
        $it = SplitIterator::fromPattern($p, 'a,b;c');

        $this->assertSame($p->searchSplit('a,b;c'), iterator_to_array($it));
        $this->assertSame(['a', 'b', 'c'], iterator_to_array($it));
    }

    public function testSearchIteratorEmptySubjectZeroWidthMatch(): void
    {
        // A pattern matching the empty string must match on an empty
        // subject; rewind must consult the core instead of bailing out.
        $it = SearchIterator::fromPattern(Pattern::fromString("''"), '');
        $it->rewind();

        $this->assertTrue($it->valid());
        $this->assertSame(0, $it->current()['_match_start']);
        $this->assertSame(0, $it->current()['_match_len']);
    }

    public function testSearchIteratorEmptySubjectNoMatch(): void
    {
        $it = SearchIterator::fromPattern(Pattern::fromString("'abc'"), '');
        $it->rewind();

        $this->assertFalse($it->valid());
        $this->assertNull($it->current());
    }

    /** @return list<int> */
    private function collectSearchStarts(SearchIterator $it): array
    {
        $starts = [];
        foreach ($it as $match) {
            $starts[] = $match['_match_start'];
            unset($match);
        }
        return $starts;
    }
}
