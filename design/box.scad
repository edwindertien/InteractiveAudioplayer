$fn=40;
//translate([0,0,22])lid();
//projection()lid();
//box();

difference(){
rotate([0,90,0])hull(){cylinder(d=8,h=4);translate([0,8,0])cylinder(d=8,h=4);translate([0,0,4])cylinder(d=4,h=2);translate([0,8,4])cylinder(d=4,h=2);};

translate([0,0,-0.5])cube([12,8,5]);
}
translate([-6,0,-4])cube([7,8,3.5]);


module lid(){
                hull(){
        translate([74,0,17])cylinder(d=8,h=2);
        translate([74,34,17])cylinder(d=8,h=2);
        translate([0,34,17])cylinder(d=8,h=2);
        translate([0,0,17])cylinder(d=8,h=2);
    }
}

module bracket(){
difference(){
    hull(){
        translate([-2,6,0])cylinder(d=2,h=5);
        translate([40,6,0])cylinder(d=2,h=5);
        translate([36,0,0])cylinder(d=4,h=5);
        translate([2,0,0])cylinder(d=4,h=5);
    }
    hull(){
        translate([4,3,-0.01])cylinder(d=4,h=55);
        translate([34,3,-0.010])cylinder(d=4,h=55);
    }
}}
        


module box(){
    translate([-1,-2,-6])rotate([90,0,90])bracket();
translate([70,-2,-6])rotate([90,0,90])bracket();

difference(){
    union(){hull(){
        translate([74,0,0])cylinder(d=10,h=19);
        translate([74,34,0])cylinder(d=10,h=19);
        translate([0,34,0])cylinder(d=10,h=19);
        translate([0,0,0])cylinder(d=10,h=19);
         translate([-9,24,13])rotate([0,90,0])hull(){cylinder(d=7.5,h=30);translate([0,4,0])cylinder(d=7.5,h=30);};
         translate([29.7,30,14.3])rotate([-90,0,0])cylinder(d=7,h=11);
 
         }
           translate([78,22,15])rotate([0,90,0])hull(){cylinder(d=8,h=6);translate([0,8,0])cylinder(d=8,h=6);};
         
     }
        hull(){
        translate([74,0,2])cylinder(d=6,h=18);
        translate([74,34,2])cylinder(d=6,h=18);
        translate([0,34,2])cylinder(d=6,h=18);
        translate([0,0,2])cylinder(d=6,h=18);
    }
            hull(){
        translate([74,0,17])cylinder(d=8,h=3);
        translate([74,34,17])cylinder(d=8,h=3);
        translate([0,34,17])cylinder(d=8,h=3);
        translate([0,0,17])cylinder(d=8,h=3);
    }
    
    translate([-12,24,13])rotate([0,90,0])hull(){cylinder(d=7.5,h=30);translate([0,4,0])cylinder(d=7.5,h=30);};
    
    translate([29.7,30,14.3])rotate([-90,0,0])cylinder(d=7,h=20);
    
    translate([70,22,14.5])cube([20,8,5]);
}


translate([14.5,17,-3])difference(){
    cylinder(d1=20,d2=24,h=3);
    cylinder(d1=16,d2=13,h=3);
}

translate([14.5+45,17,-3])difference(){
    cylinder(d1=20,d2=24,h=3);
    cylinder(d1=16,d2=13,h=3);
}
}
