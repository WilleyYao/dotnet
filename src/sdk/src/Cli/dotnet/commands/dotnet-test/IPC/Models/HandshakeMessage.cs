﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

namespace Microsoft.DotNet.Tools.Test;

internal sealed record HandshakeMessage(Dictionary<byte, string>? Properties) : IRequest, IResponse;
