<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Pattern;

/**
 * Tests for string-pattern captures (the parser path).
 *
 * The parser allocates capture registers sequentially from 0 in order of
 * @name appearance (matching Builder::cap(reg, ...)), so a single named
 * capture lands on register 0 and appears as $result['v0'].  Two named
 * captures must NOT collide (regression: parser.c hardcoded register 1,
 * so the second capture silently overwrote the first).
 */
final class StringPatternCaptureTest extends TestCase
{
    public function testSingleNamedCapture(): void
    {
        $p = Pattern::fromString("@name 'abc'");
        $r = $p->match('abc');

        $this->assertIsArray($r);
        $this->assertSame('abc', $r['v0']);
        // No spurious null-valued capture keys from an unset register.
        foreach ($r as $k => $v) {
            if (str_starts_with((string) $k, 'v') && $k !== 'v0') {
                $this->fail("unexpected capture key '$k' in result");
            }
        }
    }

    public function testTwoNamedCapturesDoNotCollide(): void
    {
        $p = Pattern::fromString("@a 'a' @b 'b'");
        $r = $p->match('ab');

        $this->assertIsArray($r);
        $this->assertSame('a', $r['v0']);
        $this->assertSame('b', $r['v1']);
    }

    public function testCaptureKeysAcrossSearchMethods(): void
    {
        $p = Pattern::fromString("@x 'a'");
        $single = $p->match('a');
        $all = $p->searchAll('aba');

        $this->assertSame('a', $single['v0']);
        $this->assertCount(2, $all);
        $this->assertSame('a', $all[0]['v0']);
        $this->assertSame('a', $all[1]['v0']);
    }

    public function testEmptyCaptureRendersNull(): void
    {
        $p = Pattern::fromString("@a ''");
        $r = $p->match('');

        $this->assertIsArray($r);
        $this->assertArrayHasKey('v0', $r);
        $this->assertNull($r['v0']);
    }

    public function testOffsetsModeCapture(): void
    {
        $p = Pattern::fromString("@name 'abc'");
        $r = $p->match('abc', ['captures' => 'offsets']);

        $this->assertIsArray($r);
        $this->assertSame([0, 3], $r['v0']);
    }

    public function testMatchResultHasStartPosition(): void
    {
        $p = Pattern::fromString("'hello'");
        $r = $p->match('hello world');

        $this->assertIsArray($r);
        $this->assertSame(0, $r['_match_start']);
        $this->assertSame(5, $r['_match_len']);
        $this->assertArrayHasKey('_output', $r);
    }
}
