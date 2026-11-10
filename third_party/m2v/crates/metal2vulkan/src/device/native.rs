#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum NativeShaderTarget {
    #[default]
    Vulkan,
    Sm53,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HalfOperation {
    Convert,
    Add,
    Subtract,
    Multiply,
    FusedMultiplyAdd,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HalfInstruction {
    Convert,
    Add2,
    Multiply2,
    FusedMultiplyAdd2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct HalfRules {
    pub instruction: HalfInstruction,
    pub nearest_even: bool,
    pub preserve_denormals: bool,
    pub overflow_to_infinity: bool,
    pub nan_payload_preserved: bool,
    pub separate_rounding: bool,
}

impl NativeShaderTarget {
    pub(crate) fn property(self, property: super::requirements::VkProperty) -> bool {
        use super::requirements::{FloatControl, FloatWidth, VkProperty};
        self == Self::Sm53
            && matches!(
                property,
                VkProperty::NativeSm53Half
                    | VkProperty::FloatControl(
                        FloatControl::SignedZeroInfNanPreserve,
                        FloatWidth::F16
                    )
            )
    }

    pub(crate) fn half(self, operation: HalfOperation) -> bool {
        self.half_rules(operation).is_some()
    }

    pub(crate) fn half_rules(self, operation: HalfOperation) -> Option<HalfRules> {
        match self {
            Self::Vulkan => None,
            Self::Sm53 => Some(HalfRules {
                instruction: match operation {
                    HalfOperation::Convert => HalfInstruction::Convert,
                    HalfOperation::Add | HalfOperation::Subtract => HalfInstruction::Add2,
                    HalfOperation::Multiply => HalfInstruction::Multiply2,
                    HalfOperation::FusedMultiplyAdd => HalfInstruction::FusedMultiplyAdd2,
                },
                nearest_even: true,
                preserve_denormals: true,
                overflow_to_infinity: true,
                nan_payload_preserved: false,
                separate_rounding: operation != HalfOperation::FusedMultiplyAdd,
            }),
        }
    }
}
