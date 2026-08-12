<?php

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Builder;
use Snobol\Pattern;
use Snobol\PatternHelper;
use Snobol\PatternCache;
use Snobol\DynamicPatternCache;
use Snobol\Table;

/**
 * Tests for the convenience API:
 *  - Pattern::match()
 *  - PatternHelper::matchOnce() / matchAll()
 *  - PatternCache / DynamicPatternCache
 *  - Text helper functions (snobol_text_*)
 *  - Builder::compile() end-to-end
 *
 * Verifies the convenience layer produces the same results as
 * the lower-level multi-step API and the new snobol_match() C API.
 */
class ConvenienceApiTest extends TestCase
{
    /* ============================================================
     *  Pattern::match() — single-step match
     * ============================================================ */

    public function testPatternMatchSimple(): void
    {
        $pat = Pattern::fromString("'hello'");
        $res = $pat->match("hello world");
        $this->assertIsArray($res);
        $this->assertEquals(5, $res['_match_len']);
    }

    public function testPatternMatchNoMatch(): void
    {
        $pat = Pattern::fromString("'xyz'");
        $this->assertFalse($pat->match("hello"));
    }

    public function testPatternMatchThrowsOnEmptyPattern(): void
    {
        $this->expectException(\Throwable::class);
        Pattern::fromString("");
    }

    public function testPatternMatchReturnsCaptures(): void
    {
        // Builder::cap(reg, sub) populates result['v<reg>'] with the matched
        // text.  This was previously broken — OP_CAP_END didn't expose captures
        // unless followed by an explicit OP_ASSIGN.  See vm.c::OP_CAP_END.
        $ast = Builder::concat([
            Builder::cap(0, Builder::span("a-z")),
            Builder::lit(" "),
            Builder::cap(1, Builder::span("a-z")),
        ]);
        $pat = Pattern::compileFromAst($ast);
        $res = $pat->match("foo bar");
        $this->assertIsArray($res);
        $this->assertEquals(7, $res['_match_len']);
        $this->assertArrayHasKey('v0', $res, "capture register 0 exposed as v0");
        $this->assertArrayHasKey('v1', $res, "capture register 1 exposed as v1");
        $this->assertEquals('foo', $res['v0']);
        $this->assertEquals('bar', $res['v1']);
    }

    public function testCapturesInAlternation(): void
    {
        // Cap in alternation: the matched branch's capture wins.
        $ast = Builder::alt(
            Builder::cap(0, Builder::lit("hello")),
            Builder::cap(0, Builder::lit("goodbye"))
        );
        $pat = Pattern::compileFromAst($ast);
        $res = $pat->match("goodbye world");
        $this->assertIsArray($res);
        $this->assertEquals('goodbye', $res['v0']);
    }

    public function testCapturesWithIdPrefix(): void
    {
        // Common pattern: extract an ID after a literal prefix.
        $ast = Builder::concat([
            Builder::lit("id:"),
            Builder::cap(0, Builder::span("0-9")),
        ]);
        $pat = Pattern::compileFromAst($ast);
        $res = $pat->match("id:12345 abc");
        $this->assertIsArray($res);
        $this->assertEquals('12345', $res['v0']);
        $this->assertEquals(8, $res['_match_len']);
    }

    /* ============================================================
     *  PatternHelper::matchOnce()
     * ============================================================ */

    public function testMatchOnceWithString(): void
    {
        $res = PatternHelper::matchOnce("'hello'", "hello world");
        $this->assertIsArray($res);
    }

    public function testMatchOnceWithAst(): void
    {
        $ast = Builder::lit("hello");
        $res = PatternHelper::matchOnce($ast, "hello world");
        $this->assertIsArray($res);
    }

    public function testMatchOnceWithPatternObject(): void
    {
        $pat = Pattern::fromString("'hello'");
        $res = PatternHelper::matchOnce($pat, "hello world");
        $this->assertIsArray($res);
    }

    public function testMatchOnceReturnsFalseOnNoMatch(): void
    {
        $res = PatternHelper::matchOnce("'xyz'", "hello");
        $this->assertFalse($res);
    }

    public function testMatchOnceWithFullOption(): void
    {
        $res = PatternHelper::matchOnce("'hello'", "hello", ['full' => true]);
        $this->assertIsArray($res);
    }

    public function testMatchOnceFullRejectsPartial(): void
    {
        $res = PatternHelper::matchOnce("'hello'", "hello world", ['full' => true]);
        $this->assertFalse($res);
    }

    public function testMatchOnceCachingSameResult(): void
    {
        $res1 = PatternHelper::matchOnce("'hello'", "hello world");
        $res2 = PatternHelper::matchOnce("'hello'", "hello world");
        $this->assertEquals($res1, $res2);
    }

    public function testMatchOnceCachingAffectedByOptions(): void
    {
        $partial = PatternHelper::matchOnce("'hello'", "hello world");
        $full    = PatternHelper::matchOnce("'hello'", "hello world", ['full' => true]);
        $this->assertNotEquals($partial, $full);
    }

    /* ============================================================
     *  PatternHelper::matchAll()
     * ============================================================ */

    public function testMatchAllReturnsArray(): void
    {
        $res = PatternHelper::matchAll("'a'", "ababab");
        $this->assertIsArray($res);
        $this->assertNotEmpty($res);
    }

    public function testMatchAllEmptyOnNoMatch(): void
    {
        $res = PatternHelper::matchAll("'xyz'", "hello");
        $this->assertIsArray($res);
        $this->assertEmpty($res);
    }

    public function testMatchAllStripsMetadata(): void
    {
        $res = PatternHelper::matchAll("'a'", "aaa");
        foreach ($res as $entry) {
            $this->assertArrayNotHasKey('_match_len', $entry);
            $this->assertArrayNotHasKey('_match_start', $entry);
        }
    }

    /* ============================================================
     *  Pattern::match() vs PatternHelper::matchOnce() parity
     * ============================================================ */

    public function testMatchAndMatchOnceEquivalent(): void
    {
        $pat = Pattern::fromString("'hello'");
        $direct = $pat->match("hello world");
        $via_helper = PatternHelper::matchOnce($pat, "hello world");
        $this->assertEquals($direct, $via_helper);
    }

    /* ============================================================
     *  Builder roundtrip — produce AST, compile, match
     * ============================================================ */

    public function testBuilderConcat(): void
    {
        $ast = Builder::concat([Builder::lit("hello"), Builder::lit(" "), Builder::lit("world")]);
        $pat = Pattern::compileFromAst($ast);
        $this->assertIsArray($pat->match("hello world"));
    }

    public function testBuilderAlt(): void
    {
        $ast = Builder::alt(Builder::lit("foo"), Builder::lit("bar"));
        $pat = Pattern::compileFromAst($ast);
        $this->assertIsArray($pat->match("foo"));
        $this->assertIsArray($pat->match("bar"));
        $this->assertFalse($pat->match("baz"));
    }

    public function testBuilderSpan(): void
    {
        $ast = Builder::span("0123456789");
        $pat = Pattern::compileFromAst($ast);
        $res = $pat->match("12345abc");
        $this->assertIsArray($res);
        $this->assertEquals(5, $res['_match_len']);
    }

    public function testBuilderLabel(): void
    {
        $ast = Builder::label('start', Builder::lit('foo'));
        $pat = Pattern::compileFromAst($ast);
        $this->assertIsArray($pat->match('foo'));
    }

    /* ============================================================
     *  PatternCache
     * ============================================================ */

    public function testPatternCacheBasic(): void
    {
        $cache = new PatternCache();
        $ast = Builder::lit("test");
        $pat = $cache->get('key1', fn() => PatternHelper::fromAst($ast));
        $this->assertInstanceOf(Pattern::class, $pat);
        $this->assertEquals(1, $cache->size());
    }

    public function testPatternCacheHitReturnsSameInstance(): void
    {
        $cache = new PatternCache();
        $ast = Builder::lit("test");
        $p1 = $cache->get('k', fn() => PatternHelper::fromAst($ast));
        $p2 = $cache->get('k', fn() => PatternHelper::fromAst($ast));
        $this->assertSame($p1, $p2);
    }

    public function testPatternCacheClear(): void
    {
        $cache = new PatternCache();
        $cache->get('k1', fn() => PatternHelper::fromAst(Builder::lit("a")));
        $cache->get('k2', fn() => PatternHelper::fromAst(Builder::lit("b")));
        $cache->clear();
        $this->assertEquals(0, $cache->size());
        $this->assertFalse($cache->has('k1'));
    }

    public function testPatternCacheEviction(): void
    {
        $cache = new PatternCache(2);
        $cache->get('k1', fn() => PatternHelper::fromAst(Builder::lit("a")));
        $cache->get('k2', fn() => PatternHelper::fromAst(Builder::lit("b")));
        $cache->get('k3', fn() => PatternHelper::fromAst(Builder::lit("c")));
        $this->assertEquals(2, $cache->size());
        $this->assertFalse($cache->has('k1'));
        $this->assertTrue($cache->has('k2'));
        $this->assertTrue($cache->has('k3'));
    }

    /* ============================================================
     *  DynamicPatternCache
     * ============================================================ */

    public function testDynamicPatternCacheCompileAndReuse(): void
    {
        $cache = new DynamicPatternCache();
        $r1 = $cache->compile("'foo'");
        $this->assertFalse($r1['cached']);
        $r2 = $cache->compile("'foo'");
        $this->assertTrue($r2['cached']);
    }

    public function testDynamicPatternCacheEvaluate(): void
    {
        $cache = new DynamicPatternCache();
        // Evaluate matches at position 0; use a pattern anchored there.
        $r = $cache->evaluate("'hello'", "hello world");
        $this->assertTrue($r['evaluated']);
    }

    public function testDynamicPatternCacheStats(): void
    {
        $cache = new DynamicPatternCache();
        $cache->compile("'a'");
        $cache->compile("'b'");
        $s = $cache->stats();
        $this->assertEquals(2, $s['size']);
        $this->assertEquals(2, $s['compile_count']);
    }

    /* ============================================================
     *  Text helpers — function form (no Text class)
     * ============================================================ */

    public function testTextSize(): void
    {
        $this->assertEquals(5, \snobol_text_size("hello"));
        $this->assertEquals(0, \snobol_text_size(""));
    }

    public function testTextUpperLower(): void
    {
        $this->assertEquals("HELLO", \snobol_text_upper("hello"));
        $this->assertEquals("hello", \snobol_text_lower("HELLO"));
    }

    public function testTextEq(): void
    {
        $this->assertTrue(\snobol_text_eq("5", "5"));
        $this->assertTrue(\snobol_text_eq("5", "5.0"));
        $this->assertFalse(\snobol_text_eq("5", "6"));
    }

    public function testTextLt(): void
    {
        $this->assertTrue(\snobol_text_lt("3", "7"));
        $this->assertFalse(\snobol_text_lt("7", "3"));
    }

    public function testTextCharOrd(): void
    {
        $this->assertEquals("A", \snobol_text_char(65));
        $this->assertEquals(65, \snobol_text_ord("A"));
    }

    /* ============================================================
     *  Table integration
     * ============================================================ */

    public function testTableSetGet(): void
    {
        $t = new Table('TEST_TBL');
        $t->set('k1', 'v1');
        $this->assertEquals('v1', $t->get('k1'));
        $this->assertTrue($t->has('k1'));
    }

    /* ============================================================
     *  Parity: convenience API and multi-step API produce same result
     * ============================================================ */

    public function testParityHelperVsBuilder(): void
    {
        $str_res = PatternHelper::matchOnce("'foo'", "foobar");
        $ast_res = PatternHelper::matchOnce(Builder::lit("foo"), "foobar");
        $this->assertEquals($str_res, $ast_res);
    }

    public function testParityWithAndWithoutFullOption(): void
    {
        $partial = PatternHelper::matchOnce("'hello'", "hello world");
        $full = PatternHelper::matchOnce("'hello'", "hello", ['full' => true]);
        $this->assertIsArray($partial);
        $this->assertIsArray($full);
        $this->assertNotFalse($partial);
        $this->assertNotFalse($full);
    }

    /* ============================================================
     *  Pattern::matchLiteral() — literal-match API
     * ============================================================ */

    public function testMatchLiteralSimple(): void
    {
        $pat = Pattern::fromString("'hello'");
        $res = $pat->matchLiteral("hello world");
        $this->assertIsArray($res);
        $this->assertTrue($res['success']);
        $this->assertEquals(0, $res['position']);
        $this->assertEquals(5, $res['length']);
    }

    public function testMatchLiteralReturnsFalseOnNoMatch(): void
    {
        $pat = Pattern::fromString("'xyz'");
        $res = $pat->matchLiteral("hello world");
        $this->assertIsArray($res);
        $this->assertFalse($res['success']);
        $this->assertEquals(0, $res['position']);
        $this->assertEquals(0, $res['length']);
    }

    public function testMatchLiteralWithNonLiteralPattern(): void
    {
        $pat = Pattern::fromString("SPAN('abc')");
        $res = $pat->matchLiteral("abc");
        $this->assertIsArray($res);
        $this->assertFalse($res['success'], "non-literal pattern should not match via matchLiteral");
    }

    public function testMatchLiteralRespectsPositionConstraint(): void
    {
        $ast = Builder::concat([Builder::pos(3), Builder::lit("the")]);
        $pat = Pattern::compileFromAst($ast);
        $res = $pat->matchLiteral("the quick brown fox");
        $this->assertIsArray($res);
        $this->assertFalse($res['success'], "position-constrained literal should not fast-path");
    }

    public function testMatchLiteralMatchesAnchoredOnly(): void
    {
        $pat = Pattern::fromString("'the'");
        $res = $pat->matchLiteral("the quick brown fox");
        $this->assertTrue($res['success']);
        $this->assertEquals(0, $res['position']);
        $this->assertEquals(3, $res['length']);
    }

    public function testMatchLiteralAgainstBuilder(): void
    {
        $ast = Builder::lit("snobol");
        $pat = Pattern::compileFromAst($ast);
        $res = $pat->matchLiteral("snobol rulez");
        $this->assertTrue($res['success']);
        $this->assertEquals(6, $res['length']);
    }

    /* ============================================================
     *  Pattern::match() literal fast path — automaton tier
     * ============================================================ */

    public function testMatchLiteralFastPath(): void
    {
        $pat = Pattern::fromString("'fastpath'");
        $res = $pat->match("fastpath");
        $this->assertIsArray($res);
        $this->assertArrayHasKey('_match_len', $res);
        $this->assertEquals(8, $res['_match_len']);
    }

    public function testMatchFastPathFallsThroughToVm(): void
    {
        $pat = Pattern::fromString("SPAN('abc') 'd'");
        $res = $pat->match("abcd");
        $this->assertIsArray($res);
        $this->assertEquals(4, $res['_match_len']);
    }

    /* ============================================================
     *  Automaton tier via Pattern::searchAll()
     * ============================================================ */

    public function testSearchAllAutomatonEligible(): void
    {
        $pat = Pattern::fromString("SPAN('abc') 'd'");
        $res = $pat->searchAll("abcd abcxd abcd");
        $this->assertIsArray($res);
        $this->assertCount(2, $res, "SPAN('abc')'d' matches at 'abcd' (2 occurrences)");
    }

    public function testSearchAllAutomatonSpanOnly(): void
    {
        $pat = Pattern::fromString("SPAN('0-9')");
        $res = $pat->searchAll("abc 123 def 4567");
        $this->assertIsArray($res);
        $this->assertCount(2, $res);
    }

    /* ============================================================
     *  PatternHelper: evalPattern / tableSubst / formattedSubst
     *  and resolve-failure contracts (coverage)
     * ============================================================ */

    public function testEvalPatternMatch(): void
    {
        $result = PatternHelper::evalPattern("'a'", "abc");
        $this->assertIsArray($result);
        $this->assertSame(1, $result['_match_len']);
    }

    public function testEvalPatternNoMatchReturnsFalse(): void
    {
        $this->assertFalse(PatternHelper::evalPattern("'z'", "abc"));
    }

    public function testEvalPatternInvalidPatternThrows(): void
    {
        $this->expectException(\Exception::class);
        PatternHelper::evalPattern("bareword", "abc");
    }

    public function testTableSubstWithTemplate(): void
    {
        $table = new Table();
        $table->set('k', 'V');
        $result = PatternHelper::tableSubst($table, "'k'", '[$v1]', 'k k');
        $this->assertSame('[] []', $result);
    }

    public function testTableSubstInvalidPatternReturnsSubject(): void
    {
        $table = new Table();
        $this->expectException(\Exception::class);
        PatternHelper::tableSubst($table, "bareword", '[x]', 'abc');
    }

    public function testFormattedSubstLiteralTemplate(): void
    {
        $result = PatternHelper::formattedSubst("'a'", '[X]', 'aaa');
        $this->assertSame('[X][X][X]', $result);
    }

    public function testFormattedSubstCaptureTemplate(): void
    {
        $pattern = Pattern::compileFromAst(Builder::cap(0, Builder::lit('a')));
        $this->assertSame('aaa', PatternHelper::formattedSubst($pattern, '$v0', 'aaa'));
        $this->assertSame('a!', PatternHelper::formattedSubst($pattern, '$v0!', 'a'));
    }

    public function testFormattedSubstResolveFailureReturnsSubject(): void
    {
        $this->assertSame('abc', PatternHelper::formattedSubst(123, 'X', 'abc'));
    }

    public function testReplaceWithDollarTemplateUsesSubst(): void
    {
        // Replacement containing '$' routes to subst; no captures -> empty
        $this->assertSame('', PatternHelper::replace("'a'", '$v0', 'aaa'));
        $this->assertSame('abc', PatternHelper::replace(123, 'X', 'abc'));
    }

    public function testMatchOnceOptionsAffectCacheKey(): void
    {
        $this->assertIsArray(PatternHelper::matchOnce("'a'", 'a', [
            'captures' => 'offsets',
            'metrics' => 1,
        ]));
        $this->assertIsArray(PatternHelper::matchOnce("'a'", 'a', ['cache' => false]));
    }

    public function testMatchOnceResolveFailures(): void
    {
        $this->assertFalse(PatternHelper::matchOnce(123, 'x'));
        $this->expectException(\Exception::class);
        PatternHelper::matchOnce(['type' => 'nope'], 'x');
    }

    public function testMatchOnceInvalidStringPatternThrows(): void
    {
        $this->expectException(\Exception::class);
        PatternHelper::matchOnce('bareword', 'x');
    }

    public function testMatchOnceFullOptionAsLong(): void
    {
        // 'full' given as int (non-zero long) counts as true
        $this->assertIsArray(PatternHelper::matchOnce("'abc'", 'abc', ['full' => 1]));
        $this->assertFalse(PatternHelper::matchOnce("'a'", 'abc', ['full' => 1]));
    }

    public function testMatchAllResolveFailureReturnsEmpty(): void
    {
        $this->assertSame([], PatternHelper::matchAll(123, 'x'));
        $this->assertSame([], PatternHelper::matchAll(new Pattern(), 'x'));
    }

    public function testSplitResolveFailureReturnsEmpty(): void
    {
        $this->assertSame([], PatternHelper::split(123, 'x'));
    }

    public function testFromAstInvalidNodeThrows(): void
    {
        // compileFromAst throws a plain Exception with the compiler's own
        // message; fromAst must propagate it (not replace it with a generic
        // ValueError).
        try {
            PatternHelper::fromAst(['type' => 'nope']);
            $this->fail('fromAst must throw for an invalid AST node');
        } catch (\Exception $e) {
            $this->assertNotSame('Failed to compile pattern from AST', $e->getMessage());
        }
    }

    public function testTypeMismatchMatrixThrowsTypeError(): void
    {
        $cases = [
            fn () => PatternHelper::fromAst('x'),
            fn () => PatternHelper::fromString("'a'", 'nope'),
            fn () => PatternHelper::matchOnce("'a'", 'a', 'nope'),
            fn () => PatternHelper::tableSubst('not-a-table', "'k'", '[v]', 'abc'),
        ];
        foreach ($cases as $case) {
            try {
                $case();
                $this->fail('Expected TypeError');
            } catch (\TypeError $e) {
                $this->assertTrue(true);
            }
        }
    }
}
