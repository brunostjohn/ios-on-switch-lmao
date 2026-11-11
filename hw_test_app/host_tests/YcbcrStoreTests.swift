import Foundation
import HWTestCore
import XCTest

/// YcbcrStore against bytes an M4 Max's Metal stored (the same texels raw_ycbcr_store_test checks the C++ rule with).
final class YcbcrStoreTests: XCTestCase {
    static let block: [SIMD4<Float>] = [
        [1060967144, 1064460635, 1057066791], [1041835799, 1051208286, 1060668499],
        [1027859266, 1053797362, 1047887546], [1060189453, 3165423067, 1051114670],
    ].map { (bits: [UInt32]) in SIMD4(bits.map(Float.init(bitPattern:)) + [1]) }

    func testCodeTargetsStoreEachBlocksRoundedMeanOfItsTexelsCodes() {
        let eight = YcbcrStore.codes(Self.block, width: 2, height: 2, max: 255, halvedDown: true, source: .drawnFloat)
        XCTAssertEqual(eight.luma, [241, 84, 103, 0])
        XCTAssertEqual(eight.chroma, [114, 104])
        let ten = YcbcrStore.codes(Self.block, width: 2, height: 2, max: 1023, halvedDown: true, source: .drawnFloat)
        XCTAssertEqual(ten.luma, [969, 336, 415, 0])
        XCTAssertEqual(ten.chroma, [458, 416])
        let pairs = YcbcrStore.codes(
            Array(Self.block[0..<2]), width: 2, height: 1, max: 255, halvedDown: false, source: .drawnFloat)
        XCTAssertEqual(pairs.chroma, [157, 113])
    }

    func testHeldHalvesRoundAsMetalHoldsThem() {
        XCTAssertEqual(YcbcrStore.code(Float(bitPattern: 1_016_107_151), .drawnFloat, max: 255), 5)
        XCTAssertEqual(YcbcrStore.code(Float(bitPattern: 1_043_509_941), .drawnFloat, max: 1023), 178)
        XCTAssertEqual(YcbcrStore.code(Float(bitPattern: 1_043_509_941), .clear, max: 1023), 179)
        XCTAssertEqual(YcbcrStore.code(.nan, .clear, max: 255), 0)
        XCTAssertEqual(YcbcrStore.code(.infinity, .drawnFloat, max: 255), 255)
    }

    func testConvertedFormatsAreMtwovsRule() {
        let video = YcbcrStore.converted(Self.block, width: 2, height: 2, full: false, halvedDown: true)
        XCTAssertEqual(video.luma, [199, 86, 77, 69])
        XCTAssertEqual(video.chroma, [132, 126])
        let full = YcbcrStore.converted(Self.block, width: 2, height: 2, full: true, halvedDown: true)
        XCTAssertEqual(full.luma, [213, 81, 71, 62])
        XCTAssertEqual(full.chroma, [132, 126])
    }
}
