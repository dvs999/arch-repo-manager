/// \brief 'main()' function which initializes the single page app.
function initPage()
{
    handleHashChange();
    queryGlobalStatus();
}

/// \brief Shows the current section and hides other sections.
function handleHashChange()
{
    if (window.preventHandlingHashChange) {
        return;
    }

    const hashParts = splitHashParts();
    const currentSectionName = hashParts.shift() || 'global-section';
    if (!currentSectionName.endsWith('-section')) {
        return;
    }

    sectionNames.forEach(function (sectionName) {
        const sectionData = sections[sectionName];
        const sectionElement = document.getElementById(sectionName + '-section');
        if (sectionElement.id === currentSectionName) {
            const sectionInitializer = sectionData.initializer;
            if (sectionInitializer === undefined || window.preventSectionInitializer || sectionInitializer(sectionElement, sectionData, hashParts)) {
                sectionElement.style.display = 'block';
            }
        } else {
            const sectionDestructor = sectionData.destructor;
            if (sectionDestructor === undefined || sectionDestructor(sectionElement, sectionData, hashParts)) {
                sectionElement.style.display = 'none';
            }
        }
        const navLinkElement = document.getElementById(sectionName + '-nav-link');
        if (sectionElement.id === currentSectionName) {
            navLinkElement.classList.add('active');
        } else {
            navLinkElement.classList.remove('active');
        }
    });
}

const sections = {
    'global': {
    },
    'package-search': {
    },
    'package-details': {
        initializer: initPackageDetails,
        state: {package: undefined},
    },
    'build-action': {
        initializer: initBuildActionsForm,
    },
    'build-action-details': {
        initializer: initBuildActionDetails,
        state: {id: undefined},
    },
};
const sectionNames = Object.keys(sections);
const status = {repoNames: undefined};