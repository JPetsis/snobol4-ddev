<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Builder;
use Snobol\Pattern;

/**
 * Source-syntax coverage for the fix-source-parser-and-compat-docs change:
 * integer builtin arguments, the README ARB quick-start, the newly parsed
 * primitives (ARBNO/BAL/FENCE/REM/RPOS/RTAB/repeat/EMIT/TABLE/assignment)
 * and the match-naming operators ('.' / '$') with their v<reg> result
 * shapes, all through Pattern::fromString.
 */
final class SourceSyntaxTest extends TestCase
{
    public function testReadmeArbQuickStart(): void
    {
        $p = Pattern::fromString("'abc' ARB 'def'");
        $r = $p->match('abc def xyz');

        $this->assertIsArray($r);
        $this->assertSame(7, $r['_match_len']);
    }

    public function testLenHonorsIntegerArgument(): void
    {
        $p = Pattern::fromString('LEN(2)');
        $r = $p->match('abc');

        $this->assertIsArray($r);
        $this->assertSame(2, $r['_match_len']);
    }

    public function testPosIntegerArgument(): void
    {
        $p = Pattern::fromString("'ab' POS(2) 'c'");
        $this->assertSame(3, $p->match('abc')['_match_len']);
        $this->assertFalse($p->match('ab'));
    }

    public function testRepeatBounds(): void
    {
        $p = Pattern::fromString("repeat('a', 2, 3)");

        $this->assertFalse($p->match('a'));
        $this->assertSame(2, $p->match('aa')['_match_len']);
        $this->assertSame(3, $p->match('aaa')['_match_len']);
        $this->assertSame(3, $p->match('aaaa')['_match_len']);
    }

    public function testArbnoEqualsStar(): void
    {
        $star = Pattern::fromString("'a'*");
        $fn = Pattern::fromString("ARBNO('a')");

        $this->assertSame(
            $star->match('aaa')['_match_len'],
            $fn->match('aaa')['_match_len']
        );
        $this->assertSame(3, $fn->match('aaa')['_match_len']);
    }

    public function testBalNestedDelimiters(): void
    {
        $p = Pattern::fromString("BAL('(', ')')");
        $this->assertSame(9, $p->match('(a (b) c)')['_match_len']);
    }

    public function testBalDefaultDelimiters(): void
    {
        $p = Pattern::fromString('BAL()');
        $this->assertSame(9, $p->match('(a (b) c)')['_match_len']);
    }

    public function testRemRtabRposFence(): void
    {
        $rem = Pattern::fromString("'ab' REM");
        $this->assertSame(6, $rem->match('abcdef')['_match_len']);

        $rtab = Pattern::fromString('RTAB(2) REM');
        $this->assertSame(6, $rtab->match('abcdef')['_match_len']);

        $rpos = Pattern::fromString("'abc' RPOS(0)");
        $this->assertSame(3, $rpos->match('abc')['_match_len']);

        $fence = Pattern::fromString("'a' FENCE");
        $this->assertSame(1, $fence->match('a')['_match_len']);
    }

    public function testDotNamingBindsSequentialRegister(): void
    {
        $p = Pattern::fromString("SPAN('a-z') . @name");
        $r = $p->match('abc12');

        $this->assertIsArray($r);
        $this->assertSame('abc', $r['v0']);
    }

    public function testDollarNamingBindsExplicitRegister(): void
    {
        $p = Pattern::fromString("'id:' SPAN('0-9') \$v1");
        $r = $p->match('id:42');

        $this->assertIsArray($r);
        $this->assertSame('42', $r['v1']);
    }

    public function testNamingBindsTighterThanConcat(): void
    {
        $p = Pattern::fromString("'a' . @x 'b'");
        $r = $p->match('ab');

        $this->assertSame(2, $r['_match_len']);
        $this->assertSame('a', $r['v0']);
    }

    public function testBuilderNameMatchesSourceNaming(): void
    {
        $fromSource = Pattern::fromString("'a' . @name");
        $fromAst = Pattern::compileFromAst(
            Builder::concat([Builder::name(Builder::lit('a'), 0)])
        );

        $this->assertSame('a', $fromSource->match('a')['v0']);
        $this->assertSame('a', $fromAst->match('a')['v0']);
    }

    public function testEmitLiteralFromSource(): void
    {
        $p = Pattern::fromString("'h' EMIT('X')");
        $r = $p->match('hi');

        $this->assertIsArray($r);
        $this->assertSame(1, $r['_match_len']);
    }

    public function testEmitCaptureFromSource(): void
    {
        $p = Pattern::fromString("@name 'ab' EMIT(@name)");
        $r = $p->match('ab');

        $this->assertIsArray($r);
        $this->assertSame('ab', $r['v0']);
    }

    public function testRegisterAssignment(): void
    {
        $p = Pattern::fromString("@x 'ab' v1 = 0");
        $r = $p->match('ab');

        $this->assertIsArray($r);
        $this->assertSame('ab', $r['v1']);
    }

    public function testQuotedNumericArgumentThrows(): void
    {
        $this->expectException(\Exception::class);
        Pattern::fromString("LEN('5')");
    }

    public function testMissingNumericArgumentThrows(): void
    {
        $this->expectException(\Exception::class);
        Pattern::fromString('LEN()');
    }

    public function testUnaryDollarIndirectReferenceThrows(): void
    {
        $this->expectException(\Exception::class);
        Pattern::fromString("\$x 'a'");
    }

    public function testInvalidNamingTargetThrows(): void
    {
        $this->expectException(\Exception::class);
        Pattern::fromString("'a' . 'b'");
    }

    public function testInvalidRepeatBoundsThrow(): void
    {
        $this->expectException(\Exception::class);
        Pattern::fromString("repeat('a', 5, 2)");
    }
}