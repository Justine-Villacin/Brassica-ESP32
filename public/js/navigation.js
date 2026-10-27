// Attaching to window so HTML bottom nav can trigger this via onclick
window.switchTab = function(viewName, element, eventObj) {
    if (window.innerWidth <= 768) {
        if (eventObj) eventObj.preventDefault();
        
        document.body.classList.remove('view-sensors', 'view-auto', 'view-history', 'view-health', 'view-export');
        document.body.classList.add('view-' + viewName);

        document.querySelectorAll('.bottom-nav .nav-item').forEach(item => {
            item.classList.remove('active');
        });
        element.classList.add('active');

        window.scrollTo({ top: 0, behavior: 'smooth' });
    }
}

window.addEventListener('DOMContentLoaded', () => {
    if (window.innerWidth <= 768) {
        document.body.classList.add('view-sensors');
    }
});

window.addEventListener('resize', () => {
    if (window.innerWidth > 768) {
        document.body.classList.remove('view-sensors', 'view-auto', 'view-history', 'view-health', 'view-export');
    } else {
        const hasActiveTab = document.body.classList.contains('view-sensors') ||
                             document.body.classList.contains('view-auto') ||
                             document.body.classList.contains('view-history') ||
                             document.body.classList.contains('view-health') ||
                             document.body.classList.contains('view-export');
        if (!hasActiveTab) {
            document.body.classList.add('view-sensors');
        }
    }
});