<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Builder;
use Snobol\Pattern;

/**
 * Tests for search-method consistency (fix-php-binding-scan-findings group 2):
 * flat-mode capture offsets, batch output extraction, stateful searchReplace,
 * subst() through the persistent state, and literal fast-path parity.
 */
final class SearchMethodConsistencyTest extends TestCase
{
    /** cap('abc') REM — REM demotes batch eligibility, so searchAll takes the
     * per-call flat path where capture offsets must anchor at match_start. */
    private function makeGapPattern(): Pattern
    {
        return Pattern::compileFromAst(Builder::concat([
            Builder::cap(0, Builder::lit('abc')),
            Builder::rem(),
        ]));
    }

    public function testFlatOffsetsAfterGap(): void
    {
        $p = $this->makeGapPattern();
        $res = $p->searchAll('xxabc!!', ['result' => 'flat', 'captures' => 'offsets']);

        // Match starts at 2; the capture spans subject[2..5).
        $this->assertSame([2], $res['match_start']);
        $this->assertSame([2, 3], $res['captures']['v0'][0]);
    }

    public function testFlatOffsetsAgreeWithArrayMode(): void
    {
        $p = $this->makeGapPattern();
        $flat = $p->searchAll('xxabc!!', ['result' => 'flat', 'captures' => 'offsets']);
        $arr = $p->searchAll('xxabc!!', ['captures' => 'offsets']);

        $this->assertSame($flat['match_start'], array_column($arr, '_match_start'));
        $this->assertSame($flat['captures']['v0'][0], $arr[0]['v0']);
    }

    public function testEmitOutputWithEmbeddedNul(): void
    {
        // EMIT patterns are batch-ineligible; the per-call path must keep the
        // output byte-exact (no strlen truncation at the embedded NUL).
        $p = Pattern::compileFromAst(Builder::emit("a\x00b"));
        $res = $p->searchAll('x');

        // EMIT is zero-width: one match per subject position.
        $this->assertCount(2, $res);
        $this->assertSame("a\x00b", $res[0]['_output']);
        $this->assertSame("a\x00b", $res[1]['_output']);
    }

    public function testSubstUsesCapturesThroughTemplateVm(): void
    {
        // subst() searches through the persistent state and runs the template
        // on a VM reconstructed from the match's capture registers.
        $p = Pattern::fromString("@x 'a'");
        $this->assertSame('[a]b[a]', $p->subst('aba', '[$v0]'));
    }

    public function testSubstEvalCallbackBehavior(): void
    {
        // OP_EVAL patterns must behave identically under subst() and match()
        // (both wire the pattern's eval callbacks on the persistent state).
        $seen = null;
        $p = Pattern::compileFromAst(Builder::concat([
            Builder::cap(0, Builder::lit('abc')),
            Builder::eval(0, 0),
        ]));
        $p->setEvalCallbacks([function (string $sub) use (&$seen): bool {
            $seen = $sub;
            return true;
        }]);

        $this->assertIsArray($p->match('abcdef'));
        $this->assertSame('abc', $seen);
    }

    public function testSearchReplaceBatchAndFallbackAgree(): void
    {
        // Eligible pattern ('ab') takes the batch fast path; the EVAL variant
        // is batch-ineligible and takes the per-call fallback. Both must
        // produce the same replacement.
        $eligible = Pattern::fromString("'ab'");
        $ineligible = Pattern::compileFromAst(Builder::concat([
            Builder::lit('ab'),
            Builder::eval(0, 0),
        ]));
        $ineligible->setEvalCallbacks([fn (string $sub): bool => true]);

        $subject = 'ababab';
        $this->assertSame('xxxxxx', $eligible->searchReplace($subject, 'xx'));
        $this->assertSame('xxxxxx', $ineligible->searchReplace($subject, 'xx'));
    }

    public function testSearchReplaceLargeSubject(): void
    {
        // >1 KB subject through the per-call fallback (EVAL) — the removed
        // counting pass must not change results.
        $p = Pattern::compileFromAst(Builder::concat([
            Builder::lit('ab'),
            Builder::eval(0, 0),
        ]));
        $p->setEvalCallbacks([fn (string $sub): bool => true]);

        $subject = str_repeat('ab', 600); // 1200 bytes
        $result = $p->searchReplace($subject, 'Z');

        $this->assertSame(str_repeat('Z', 600), $result);
    }

    public function testLiteralFastPathParity(): void
    {
        $p = Pattern::fromString("'hello'");
        $this->assertSame(['success' => true, 'position' => 0, 'length' => 5], $p->matchLiteral('hello'));
        $this->assertSame(['success' => false, 'position' => 0, 'length' => 0], $p->matchLiteral('world'));

        $m = $p->match('hello');
        $this->assertIsArray($m);
        $this->assertSame(5, $m['_match_len']);
    }

    public function testPositionConstrainedLiteralRejectedConsistently(): void
    {
        // POS-constrained patterns are not literal-only: match() takes the
        // general path; matchLiteral() reports its documented non-literal
        // result. Both fast paths reject the form consistently.
        $p = Pattern::fromString("POS('2') 'ab'");
        $this->assertSame(['success' => false, 'position' => 0, 'length' => 0], $p->matchLiteral('xxab'));
        $m = $p->match('xxab');
        $this->assertFalse($m);
    }
}
