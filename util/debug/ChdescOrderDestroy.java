import java.io.DataInput;
//import java.io.IOException;

public class ChdescOrderDestroy extends Opcode
{
	private final int order;
	
	public ChdescOrderDestroy(int order)
	{
		this.order = order;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_ORDER_DESTROY: order = " + SystemState.hex(order);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ORDER_DESTROY, "KDB_CHDESC_ORDER_DESTROY", ChdescOrderDestroy.class);
		factory.addParameter("order", 4);
		return factory;
	}
}
