﻿// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#nullable disable

using System.Collections.Immutable;
using System.Diagnostics;

namespace Microsoft.CodeAnalysis.CSharp.Symbols
{
    /// <summary>
    /// Base class for event accessors - synthesized and user defined.
    /// </summary>
    internal abstract class SourceEventAccessorSymbol : SourceMemberMethodSymbol
    {
        private readonly SourceEventSymbol _event;
        private readonly string _name;
        private readonly ImmutableArray<MethodSymbol> _explicitInterfaceImplementations;

        private readonly ImmutableArray<ParameterSymbol> _parameters;
        private TypeWithAnnotations _lazyReturnType;

        public SourceEventAccessorSymbol(
            SourceEventSymbol @event,
            SyntaxReference syntaxReference,
            Location location,
            EventSymbol explicitlyImplementedEventOpt,
            string aliasQualifierOpt,
            bool isAdder,
            bool isIterator,
            bool isNullableAnalysisEnabled,
            bool isExpressionBodied)
            : base(@event.containingType, syntaxReference, location, isIterator,
                   (@event.Modifiers, MakeFlags(
                                                isAdder ? MethodKind.EventAdd : MethodKind.EventRemove,
                                                RefKind.None,
                                                @event.Modifiers,
                                                returnsVoid: false, // until we learn otherwise (in LazyMethodChecks).
                                                returnsVoidIsSet: false,
                                                isExpressionBodied: isExpressionBodied,
                                                isExtensionMethod: false,
                                                isNullableAnalysisEnabled: isNullableAnalysisEnabled,
                                                isVarArg: false,
                                                isExplicitInterfaceImplementation: @event.IsExplicitInterfaceImplementation,
                                                hasThisInitializer: false)))
        {
            _event = @event;
            _parameters = ImmutableArray.Create<ParameterSymbol>(new SynthesizedEventAccessorValueParameterSymbol(this, 0));

            string name;
            ImmutableArray<MethodSymbol> explicitInterfaceImplementations;
            if ((object)explicitlyImplementedEventOpt == null)
            {
                name = SourceEventSymbol.GetAccessorName(@event.Name, isAdder);
                explicitInterfaceImplementations = ImmutableArray<MethodSymbol>.Empty;
            }
            else
            {
                MethodSymbol implementedAccessor = isAdder ? explicitlyImplementedEventOpt.AddMethod : explicitlyImplementedEventOpt.RemoveMethod;
                string accessorName = (object)implementedAccessor != null ? implementedAccessor.Name : SourceEventSymbol.GetAccessorName(explicitlyImplementedEventOpt.Name, isAdder);

                name = ExplicitInterfaceHelpers.GetMemberName(accessorName, explicitlyImplementedEventOpt.ContainingType, aliasQualifierOpt);
                explicitInterfaceImplementations = (object)implementedAccessor == null ? ImmutableArray<MethodSymbol>.Empty : ImmutableArray.Create<MethodSymbol>(implementedAccessor);
            }

            _explicitInterfaceImplementations = explicitInterfaceImplementations;
            _name = GetOverriddenAccessorName(@event, isAdder) ?? name;
        }

        public override string Name
        {
            get { return _name; }
        }

        internal override bool IsExplicitInterfaceImplementation
        {
            get { return _event.IsExplicitInterfaceImplementation; }
        }

        public override ImmutableArray<MethodSymbol> ExplicitInterfaceImplementations
        {
            get { return _explicitInterfaceImplementations; }
        }

        public sealed override bool AreLocalsZeroed
            => !_event.HasSkipLocalsInitAttribute && base.AreLocalsZeroed;

        public SourceEventSymbol AssociatedEvent
        {
            get { return _event; }
        }

        public sealed override Symbol AssociatedSymbol
        {
            get { return _event; }
        }

        protected sealed override void MethodChecks(BindingDiagnosticBag diagnostics)
        {
            // CONSIDER: currently, we're copying the custom modifiers of the event overridden
            // by this method's associated event (by using the associated event's type, which is
            // copied from the overridden event).  It would be more correct to copy them from
            // the specific accessor that this method is overriding (as in SourceMemberMethodSymbol).

            if (_lazyReturnType.IsDefault)
            {
                CSharpCompilation compilation = this.DeclaringCompilation;
                Debug.Assert(compilation != null);

                // NOTE: LazyMethodChecks calls us within a lock, so we use regular assignments,
                // rather than Interlocked.CompareExchange.
                if (_event.IsWindowsRuntimeEvent)
                {
                    TypeSymbol eventTokenType = compilation.GetWellKnownType(WellKnownType.System_Runtime_InteropServices_WindowsRuntime_EventRegistrationToken);
                    Binder.ReportUseSite(eventTokenType, diagnostics, this.Location);

                    if (this.MethodKind == MethodKind.EventAdd)
                    {
                        // EventRegistrationToken add_E(EventDelegate d);

                        _lazyReturnType = TypeWithAnnotations.Create(eventTokenType);
                        this.SetReturnsVoid(returnsVoid: false);
                    }
                    else
                    {
                        Debug.Assert(this.MethodKind == MethodKind.EventRemove);

                        // void remove_E(EventRegistrationToken t);

                        TypeSymbol voidType = compilation.GetSpecialType(SpecialType.System_Void);
                        Binder.ReportUseSite(voidType, diagnostics, this.Location);
                        _lazyReturnType = TypeWithAnnotations.Create(voidType);
                        this.SetReturnsVoid(returnsVoid: true);
                    }
                }
                else
                {
                    // void add_E(EventDelegate d);
                    // void remove_E(EventDelegate d);

                    TypeSymbol voidType = compilation.GetSpecialType(SpecialType.System_Void);
                    Binder.ReportUseSite(voidType, diagnostics, this.Location);
                    _lazyReturnType = TypeWithAnnotations.Create(voidType);
                    this.SetReturnsVoid(returnsVoid: true);
                }
            }
        }

        public sealed override bool ReturnsVoid
        {
            get
            {
                LazyMethodChecks();
                Debug.Assert(!_lazyReturnType.IsDefault);
                return base.ReturnsVoid;
            }
        }

        public sealed override TypeWithAnnotations ReturnTypeWithAnnotations
        {
            get
            {
                LazyMethodChecks();
                Debug.Assert(!_lazyReturnType.IsDefault);
                return _lazyReturnType;
            }
        }

        public sealed override ImmutableArray<CustomModifier> RefCustomModifiers
        {
            get
            {
                return ImmutableArray<CustomModifier>.Empty; // Same as base, but this is clear and explicit.
            }
        }

        public sealed override ImmutableArray<ParameterSymbol> Parameters
        {
            get
            {
                return _parameters;
            }
        }

        public sealed override ImmutableArray<TypeParameterSymbol> TypeParameters
        {
            get { return ImmutableArray<TypeParameterSymbol>.Empty; }
        }

        public sealed override ImmutableArray<ImmutableArray<TypeWithAnnotations>> GetTypeParameterConstraintTypes()
            => ImmutableArray<ImmutableArray<TypeWithAnnotations>>.Empty;

        public sealed override ImmutableArray<TypeParameterConstraintKind> GetTypeParameterConstraintKinds()
            => ImmutableArray<TypeParameterConstraintKind>.Empty;

        internal Location Location
        {
            get
            {
                Debug.Assert(this.Locations.Length == 1);
                return this.GetFirstLocation();
            }
        }

        protected string GetOverriddenAccessorName(SourceEventSymbol @event, bool isAdder)
        {
            if (this.IsOverride)
            {
                // NOTE: What we'd really like to do is ask for the OverriddenMethod of this symbol.
                // Unfortunately, we can't do that, because it would inspect the signature of this
                // method, which depends on whether @event is a WinRT event, which depends on
                // interface implementation, which we can't check during construction of the 
                // member list of the type containing this accessor (infinite recursion).  Instead,
                // we inline part of the implementation of OverriddenMethod - we look for the
                // overridden event (which does not depend on WinRT-ness) and then grab the corresponding
                // accessor.
                EventSymbol overriddenEvent = @event.OverriddenEvent;
                if ((object)overriddenEvent != null)
                {
                    // If this accessor is overriding an accessor from metadata, it is possible that
                    // the name of the overridden accessor doesn't follow the C# add_X/remove_X pattern.
                    // We should copy the name so that the runtime will recognize this as an override.
                    MethodSymbol overriddenAccessor = overriddenEvent.GetOwnOrInheritedAccessor(isAdder);
                    return (object)overriddenAccessor == null ? null : overriddenAccessor.Name;
                }
            }

            return null;
        }

        internal sealed override int TryGetOverloadResolutionPriority()
        {
            return 0;
        }

#nullable enable
        protected abstract override SourceMemberMethodSymbol? BoundAttributesSource { get; }

        public sealed override MethodSymbol? PartialImplementationPart => _event is { IsPartialDefinition: true, OtherPartOfPartial: { } other }
            ? (MethodKind == MethodKind.EventAdd ? other.AddMethod : other.RemoveMethod)
            : null;

        public sealed override MethodSymbol? PartialDefinitionPart => _event is { IsPartialImplementation: true, OtherPartOfPartial: { } other }
            ? (MethodKind == MethodKind.EventAdd ? other.AddMethod : other.RemoveMethod)
            : null;

        internal bool IsPartialDefinition => _event.IsPartialDefinition;

        internal bool IsPartialImplementation => _event.IsPartialImplementation;

        public sealed override bool IsExtern => PartialImplementationPart is { } implementation ? implementation.IsExtern : base.IsExtern;
    }
}
