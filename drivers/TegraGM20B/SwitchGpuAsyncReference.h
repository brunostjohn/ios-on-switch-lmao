// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOUserClient.h>
#include <string.h>

class SwitchGpuAsyncReference
{
public:
	explicit SwitchGpuAsyncReference(const IOExternalMethodArguments *arguments)
	{
		if (arguments && arguments->asyncReference && arguments->asyncReferenceCount) {
			const uint32_t count = arguments->asyncReferenceCount < kOSAsyncRef64Count
						       ? arguments->asyncReferenceCount
						       : kOSAsyncRef64Count;
			memcpy(reference_, arguments->asyncReference,
			       count * sizeof(reference_[0]));
		}
	}

	~SwitchGpuAsyncReference()
	{
		IOUserClient::releaseAsyncReference64(reference_);
	}

	void take(OSAsyncReference64 output)
	{
		memcpy(output, reference_, sizeof(reference_));
		reference_[0] = 0;
	}

private:
	OSAsyncReference64 reference_ = {};
};
