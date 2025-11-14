extends CharacterBody2D

# Simple player script that responds to directional input
# Used for smoke testing the headless test infrastructure

const SPEED = 300.0

func _ready():
	print("Player: Ready at position ", position)

func _physics_process(delta):
	# Get input direction
	var direction = Vector2.ZERO
	
	if Input.is_action_pressed("ui_right"):
		direction.x += 1
	if Input.is_action_pressed("ui_left"):
		direction.x -= 1
	if Input.is_action_pressed("ui_down"):
		direction.y += 1
	if Input.is_action_pressed("ui_up"):
		direction.y -= 1
	
	# Normalize diagonal movement
	if direction.length() > 0:
		direction = direction.normalized()
	
	# Apply movement
	velocity = direction * SPEED
	move_and_slide()

